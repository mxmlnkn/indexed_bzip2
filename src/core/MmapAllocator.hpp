#pragma once

#include <chrono>
#include <limits>
#include <new>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef __linux__

#include <common.hpp>

#include <sys/mman.h>


static std::atomic<size_t> sumAllocated{ 0 };
static void* allocatedMemory{ nullptr };
static constexpr size_t allocatedMemorySize{ 5_Gi };
static std::atomic_flag hasBeenAllocated{ false };
static std::atomic<size_t> usedUpMemory{ 0 };


/**
 * Allocates with mmap and deallocates with munmap. Data allocated as such can be used with vmsplice.
 * The standard allocator won't do because deallocating it might not call munmap and instead it might
 * get reused leading to wrong data written out with vmsplice.
 */
template<typename ElementType>
class MmapAllocator
{
public:
    using value_type = ElementType;

    [[nodiscard]] constexpr ElementType*
    allocate( std::size_t nElementsToAllocate )
    {
        // Mostly 32 KiB for base64.gz
        //std::cerr << "Allocate " << nElementsToAllocate * sizeof( ElementType ) / 1024 << " KiB\n";

        if ( !hasBeenAllocated.test_and_set() ) {
            auto* const result = mmap( nullptr, allocatedMemorySize,
                                       PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS,
                                       /* fd */ -1, /* offset */ 0 );
            if ( result == (void*)-1 ) {
                throw std::bad_alloc();
            }

            allocatedMemory = result;
        } else {
            while ( allocatedMemory == nullptr ) {
                using namespace std::chrono_literals;
                std::this_thread::sleep_for( 1us );
            }
        }

        auto const nBytesToAllocate = nElementsToAllocate * sizeof( ElementType );
        if ( usedUpMemory + nBytesToAllocate > allocatedMemorySize ) {
            throw std::bad_alloc();
        }

        const auto offset = usedUpMemory.fetch_add( nBytesToAllocate );
        if ( offset >= allocatedMemorySize ) {
            throw std::bad_alloc();
        }

        sumAllocated += nBytesToAllocate;
        return reinterpret_cast<ElementType*>( reinterpret_cast<char*>( allocatedMemory ) + offset );
    }

    constexpr void
    deallocate( ElementType* allocatedPointer,
                std::size_t  nElementsAllocated )
    {
        //munmap( allocatedPointer, nBytesAllocated );
    }
};


template<typename T>
using SplicableVector = std::vector<T, MmapAllocator<T> >;

#else

template<typename T>
using SplicableVector = std::vector<T>;

#endif
