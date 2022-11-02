#pragma once

#include <chrono>
#include <limits>
#include <list>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef __linux__

#include <common.hpp>

#include <sys/mman.h>


inline static std::atomic<size_t> mmapCallCount{ 0 };
inline static std::atomic<size_t> munmapCallCount{ 0 };


[[nodiscard]] inline static void*
allocateWithMmap( size_t size )
{
    auto* const result = mmap( nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                               /* fd */ -1, /* offset */ 0 );
    if ( result == (void*)-1 ) {
        throw std::bad_alloc();
    }
    ++mmapCallCount;
    return result;
}


inline static void
deallocateWithMunmap( void*  pointer,
                      size_t size )
{
    if ( pointer == nullptr ) {
        return;
    }
    ++munmapCallCount;
    munmap( pointer, size );
}


class AtomicLock
{
public:
    void
    lock()
    {
        /* Wait until we successfully have set the flag ourselves.
         * Test with relaxed memory order first to improve speed because the modifying exchange has to
         * lock the cache line. */
        while ( m_flag.load( std::memory_order_relaxed ) || m_flag.exchange( true, std::memory_order_acquire ) ) {
            using namespace std::chrono_literals;
            std::this_thread::sleep_for( 10ns );
        }
    }

    void
    unlock()
    {
        m_flag.store( false, std::memory_order_release );
    }

private:
    std::atomic<bool> m_flag{ false };
};


/**
 * Allocates with mmap and deallocates with munmap. Data allocated as such can be used with vmsplice.
 * The standard allocator won't do because deallocating it might not call munmap and instead it might
 * get reused leading to wrong data written out with vmsplice.
 * The allocated anonymous pages are page-alignede, which should be sufficiently aligned for any kind
 * of data or optimization.
 */
template<typename ElementType>
class MmapAllocator
{
private:
    /**
     * Test with:
     * > m pragzip && time src/tools/pragzip -P 0 -d -c 4GiB-base64.gz | wc -l
     *
     * Result for when m_usedChunks was never set and therefore munmap was never called.
     * Note that this also leads to a lot of slabs that have to be processed in each allocate/deallocate.
     * @verbatim
     * Slab Size / MiB | Bandwidth / MB/s         | Mmap Calls | Munmap Calls
     * ----------------+--------------------------+------------+--------------
     *    1            |  668  689  690  697  692 | 7940       | 0
     *    2            | 1124  982 1116 1115 1120 | 3972       | 0
     *    4            | 1546 1595 1578 1583 1608 | 1988       | 0
     *    8            | 2079 2047 2081 2058 2084 | 995        | 0
     *   16            | 2453 2410 2404 2446 2343 | 499        | 0
     *   32            | 2687 2697 2570 2625 2638 | 251        | 0
     *   64            | 2809 2708 2725 2748 2729 | 127        | 0
     *  128            | 2796 2793 2832 2785 2742 | 65         | 0
     *  256            | 2859 2849 2820 2764 2791 | 34         | 0
     *  512            | 2866 2869 2855 2845 2796 | 19         | 0
     * @endverbatim
     *
     * With m_usedChunks bug fixed so that munmap will be called.
     * @verbatim
     * Slab Size / MiB | Bandwidth / MB/s         | Mmap Calls | Munmap Calls
     * ----------------+--------------------------+------------+--------------
     *    1            | 1913 1901 1867 1864 1894 | 7940       | 7936
     *    8            | 2061 2066 2069 2060 1983 | 995        | 991
     * @endverbatim
     * The munmap calls actually slow things down and commenting them out yields the 2.8 GB/s again :(
     */
    static constexpr size_t SLAB_SIZE = 128_Mi;

    class LockFreeSlab
    {
    public:
        LockFreeSlab( size_t chunkCount,
                      size_t chunkSize ) :
            m_chunkCount( chunkCount ),
            m_chunkSize( chunkSize )
        {}

        ~LockFreeSlab()
        {
            deallocateWithMunmap( m_data.exchange( nullptr ), m_chunkCount * m_chunkSize );
        }

        [[nodiscard]] size_t
        chunkSize() const noexcept
        {
            return m_chunkSize;
        }

        [[nodiscard]] size_t
        chunkCount() const noexcept
        {
            return m_chunkSize;
        }

        /**
         * @return a pointer to a memory sized @ref chunkSize or nullptr if there is no free chunk.
         */
        [[nodiscard]] void*
        allocate()
        {
            /* This is only to avoid overflows for very frequent checks by avoiding the increment for failing checks. */
            if ( m_usedChunkCount >= m_chunkCount ) {
                return nullptr;
            }

            ensureAllocatedSlab();

            const auto chunkIndex = m_usedChunkCount.fetch_add( 1 );
            /* Second check because it might have been incremented by another thread since the last check. */
            if ( chunkIndex >= m_chunkCount ) {
                return nullptr;
            }

            return reinterpret_cast<void*>( reinterpret_cast<uintptr_t>( m_data.load() ) + chunkIndex * m_chunkSize );
        }

        /**
         * @return True if the given pointer belongs to this slab.
         */
        [[nodiscard]] bool
        deallocate( void* pointer )
        {
            const auto chunkIndex = static_cast<size_t>( reinterpret_cast<uintptr_t>( pointer )
                                                         - reinterpret_cast<uintptr_t>( m_data.load() ) ) / m_chunkSize;
            if ( ( m_data == nullptr ) || ( chunkIndex >= m_chunkCount ) ) {
                return false;
            }

            if ( ++m_freedChunkCount == m_chunkCount ) {
                deallocateWithMunmap( m_data.exchange( nullptr ), m_chunkCount * m_chunkSize );
            }
            //if ( ++m_freedChunkCount == m_chunkCount ) {
            //    /* Even slower than simply calling munmap here :( */
            //    madvise( m_data.load(), m_chunkCount * m_chunkSize, MADV_DONTNEED );
            //}
            return true;
        }

        [[nodiscard]] bool
        emptied() const noexcept
        {
            return m_freedChunkCount >= m_chunkCount;
        }

    private:
        void
        ensureAllocatedSlab()
        {
            if ( m_toBeAllocated.test_and_set() ) {
                while ( m_data == nullptr ) {
                    using namespace std::chrono_literals;
                    std::this_thread::sleep_for( 10ns );
                }
            } else {
                m_data = allocateWithMmap( m_chunkCount * m_chunkSize );
            }
        }

    private:
        const size_t m_chunkCount;
        const size_t m_chunkSize;

        /** The thread setting this to true must initialize @ref memory to something other than nullptr. */
        std::atomic_flag m_toBeAllocated{ ATOMIC_FLAG_INIT };
        /* std::atomic<void*>{}.is_lock_free() returns true on my system.
         * It is atomic so that the spin-wait loop for m_data != nullptr is well-defined.
         * It basically also acts as a "hasBeenAllocated" flag. Similar things hold true for unmapping. */
        std::atomic<void*> m_data{ nullptr };

        std::atomic<size_t> m_usedChunkCount{ 0 };
        std::atomic<size_t> m_freedChunkCount{ 0 };
    };

    class ThreadUnsafeSlab
    {
    public:
        ThreadUnsafeSlab( size_t chunkCount,
                          size_t chunkSize ) :
            m_chunkCount( chunkCount ),
            m_chunkSize( chunkSize )
        {}

        [[nodiscard]] size_t
        chunkSize() const noexcept
        {
            return m_chunkSize;
        }

        [[nodiscard]] size_t
        chunkCount() const noexcept
        {
            return m_chunkSize;
        }

        /**
         * @return a pointer to a memory sized @ref chunkSize or nullptr if there is no free chunk.
         */
        [[nodiscard]] void*
        allocate()
        {
            if ( m_usedChunkCount >= m_chunkCount ) {
                return nullptr;
            }

            const auto chunkIndex = m_usedChunkCount;
            ++m_usedChunkCount;

            if ( m_data == nullptr ) {
                m_data = allocateWithMmap( m_chunkCount * m_chunkSize );
            }

            m_usedChunks[chunkIndex] = true;
            return reinterpret_cast<void*>( reinterpret_cast<uintptr_t>( m_data ) + chunkIndex * m_chunkSize );
        }

        /**
         * @return True if the given pointer belongs to this slab.
         */
        [[nodiscard]] bool
        deallocate( void* pointer )
        {
            const auto chunkIndex = static_cast<size_t>( reinterpret_cast<uintptr_t>( pointer )
                                                         - reinterpret_cast<uintptr_t>( m_data ) ) / m_chunkSize;
            if ( ( m_data == nullptr ) || ( chunkIndex >= m_chunkCount ) ) {
                return false;
            }

            if ( m_usedChunks[chunkIndex] ) {
                m_usedChunks[chunkIndex] = false;
                ++m_freedChunkCount;
                if ( m_freedChunkCount >= m_chunkCount ) {
                    deallocateWithMunmap( m_data, m_chunkCount * m_chunkSize );
                    m_data = nullptr;
                }
            }

            return true;
        }

        [[nodiscard]] bool
        emptied() const noexcept
        {
            return m_freedChunkCount >= m_chunkCount;
        }

    private:
        size_t m_chunkCount;
        size_t m_chunkSize;

        void* m_data{ nullptr };

        size_t m_usedChunkCount{ 0 };
        size_t m_freedChunkCount{ 0 };
        /** Assuming no double-frees, this is not necessary because we do not want to reuse freed chunks! */
        std::vector<bool> m_usedChunks{ std::vector<bool>( m_chunkCount, false ) };
    };

    struct BucketWithLock
    {
    private:
        using Slab = ThreadUnsafeSlab;
        using Slabs = std::vector<Slab>;

    public:
        explicit
        BucketWithLock( size_t chunkSize ) :
            m_chunkSize( chunkSize )
        {}

        [[nodiscard]] size_t
        chunkSize() const noexcept
        {
            return m_chunkSize;
        }

        /**
         * @return a pointer to a memory sized @p size.
         */
        [[nodiscard]] void*
        allocate()
        {
            const std::scoped_lock lock{ m_mutex };

            if ( m_slabs.empty() ) {
                m_slabs.emplace_back( SLAB_SIZE / m_chunkSize, m_chunkSize );
            }

            auto* chunk = m_slabs.back().allocate();
            if ( chunk == nullptr ) {
                m_slabs.emplace_back( SLAB_SIZE / m_chunkSize, m_chunkSize );
                chunk = m_slabs.back().allocate();
            }

            if ( chunk != nullptr ) {
                return chunk;
            }

            throw std::bad_alloc();
        }

        void
        deallocate( void* pointer )
        {
            /* Go over all slabs to find the containing one and deallocate there.
             * There is a tradeoff here. We do not want the slabs to only grow because deallocating would
             * become slower and slower not to mention the leaking memory. But, we also do not want to
             * erase a slab as soon as it is empty and move all elements thereafter because this is potentially slow. */
            std::scoped_lock lock{ m_mutex };

            for ( auto& slab : m_slabs ) {
                if ( slab.deallocate( pointer ) ) {
                    if ( slab.emptied() ) {
                        ++m_emptiedSlabs;
                    }
                    break;
                }
            }

            /* This should be effectively linear because it is only done every N/2 time and the algorithm executed
             * with that frequency is O(N) leading to O(N / (N/2)) = O(1) on average. */
            if ( 2 * m_emptiedSlabs >= m_slabs.size() ) {
                m_slabs.erase( std::remove_if( m_slabs.begin(), m_slabs.end(),
                                               [] ( const auto& slab ) { return slab.emptied(); } ),
                               m_slabs.end() );
            }
        }

    private:
        const size_t m_chunkSize;  /** Slabs will be allocated with this chunk size. */

        Slabs m_slabs;
        size_t m_emptiedSlabs{ 0 };
        AtomicLock m_mutex;
    };

    struct BucketUsingLockFreeSlab
    {
    private:
        using Slab = LockFreeSlab;
        using Slabs = std::list<Slab>;

    public:
        explicit
        BucketUsingLockFreeSlab( size_t chunkSize ) :
            m_chunkSize( chunkSize )
        {}

        [[nodiscard]] size_t
        chunkSize() const noexcept
        {
            return m_chunkSize;
        }

        /**
         * @return a pointer to a memory sized @p size.
         */
        [[nodiscard]] void*
        allocate()
        {
            if ( m_slabs.empty() ) {
                const std::scoped_lock lock{ m_mutex };
                m_slabs.emplace_back( SLAB_SIZE / m_chunkSize, m_chunkSize );
            }

            auto* chunk = m_slabs.back().allocate();
            if ( chunk == nullptr ) {
                {
                    const std::scoped_lock lock{ m_mutex };
                    m_slabs.emplace_back( SLAB_SIZE / m_chunkSize, m_chunkSize );
                }
                chunk = m_slabs.back().allocate();
            }

            if ( chunk != nullptr ) {
                return chunk;
            }

            throw std::bad_alloc();
        }

        void
        deallocate( void* pointer )
        {
            /* Go over all slabs to find the containing one and deallocate there. */
            for ( auto& slab : m_slabs ) {
                if ( slab.deallocate( pointer ) ) {
                    if ( slab.emptied() ) {
                        ++m_emptiedSlabs;
                    }
                    break;
                }
            }

            /* This should be effectively linear because it is only done every N/2 time and the algorithm executed
             * with that frequency is O(N) leading to O(N / (N/2)) = O(1) on average. */
            if ( ( m_emptiedSlabs > 10 ) && ( 2 * m_emptiedSlabs >= m_slabs.size() ) ) {
                const std::scoped_lock lock{ m_mutex };
                if ( ( m_emptiedSlabs > 10 ) && ( 2 * m_emptiedSlabs >= m_slabs.size() ) ) {
                    for ( auto slab = m_slabs.begin(); slab != m_slabs.end(); ) {
                        if ( slab->emptied() ) {
                            slab = m_slabs.erase( slab );
                        } else {
                            ++slab;
                        }
                    }
                }
                m_emptiedSlabs = 0;
            }
        }

    private:
        const size_t m_chunkSize;  /** Slabs will be allocated with this chunk size. */

        Slabs m_slabs;
        size_t m_emptiedSlabs{ 0 };
        AtomicLock m_mutex;
    };

    using Bucket = BucketUsingLockFreeSlab;

    class MemoryPool
    {
    public:
        [[nodiscard]] void*
        allocate( size_t size )
        {
            if ( const auto bucketIndex = bucketBySize( size ); bucketIndex.has_value() ) {
                return m_buckets[*bucketIndex].allocate();
            }
            return allocateWithMmap( size );
        }

        void
        deallocate( void*  pointer,
                    size_t size ) noexcept
        {
            if ( const auto bucketIndex = bucketBySize( size ); bucketIndex.has_value() ) {
                m_buckets[*bucketIndex].deallocate( pointer );
            } else {
                deallocateWithMunmap( pointer, size );
            }
        }

    private:
        [[nodiscard]] std::optional<size_t>
        bucketBySize( size_t size ) const
        {
            const auto match = std::find_if( m_buckets.begin(), m_buckets.end(),
                                             [size] ( const auto& bucket ) { return bucket.chunkSize() >= size; } );
            if ( match == m_buckets.end() ) {
                return std::nullopt;
            }
            return static_cast<size_t>( match - m_buckets.begin() );
        }

    private:
        std::array<Bucket, 7> m_buckets = {
            Bucket( /* chunk size */ 8 ),
            Bucket( /* chunk size */ 64 ),
            Bucket( /* chunk size */ 256 ),
            Bucket( /* chunk size */ 1_Ki ),
            Bucket( /* chunk size */ 8_Ki ),
            Bucket( /* chunk size */ 64_Ki ),
            Bucket( /* chunk size */ 256_Ki ),
        };
    };

public:
    using value_type = ElementType;

    [[nodiscard]] constexpr ElementType*
    allocate( std::size_t nElementsToAllocate )
    {
        // Mostly 32 KiB for base64.gz
        //std::cerr << "Allocate " << nElementsToAllocate * sizeof( ElementType ) / 1024 << " KiB\n";

        auto const nBytesToAllocate = nElementsToAllocate * sizeof( ElementType );
        auto* const result = pool.allocate( nBytesToAllocate );
        sumAllocated += nBytesToAllocate;
        return reinterpret_cast<ElementType*>( result );
    }

    constexpr void
    deallocate( ElementType* allocatedPointer,
                std::size_t  nElementsAllocated )
    {
        auto const nBytesToAllocated = nElementsAllocated * sizeof( ElementType );
        pool.deallocate( allocatedPointer, nBytesToAllocated );
    }

public:
    inline static MemoryPool pool;

    /* Statistics */
    inline static std::atomic<size_t> sumAllocated{ 0 };
};


template<typename T>
using SplicableVector = std::vector<T, MmapAllocator<T> >;

#else

template<typename T>
using SplicableVector = std::vector<T>;

#endif
