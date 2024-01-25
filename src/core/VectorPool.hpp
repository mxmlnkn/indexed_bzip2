#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "common.hpp"  // now, duration


template<typename Container>
class VectorPool :
    public std::enable_shared_from_this<VectorPool<Container> >
{
public:
    /**
     * These containers are only created via VectorPool::create anyway, so we don't have to worry
     * about having to redeclare and redirect to all possible std::vector constructors when inheriting.
     */
    class WrappedContainer :
        public Container
    {
    public:
        WrappedContainer( Container&&               container,
                          std::weak_ptr<VectorPool> pool ) :
            Container( std::move( container ) ),
            m_pool( std::move( pool ) )
        {}

        /**
         * Beware! Declaring a destructor leads to move-constructors and assignment to not be generated
         * but copying still works and will be used instead! Leading to errors because the reserved capacity
         * is not copied!
         */
        ~WrappedContainer()
        {
            if ( const auto sharedPool = m_pool.lock() ) {
                sharedPool->reuse( std::move( static_cast<Container&>( *this ) ) );
            }
        }

        WrappedContainer( WrappedContainer&& other ) = default;
        WrappedContainer( const WrappedContainer& other ) = delete;
        WrappedContainer& operator=( WrappedContainer&& other ) = default;
        WrappedContainer& operator=( const WrappedContainer& other ) = delete;
    private:
        const std::weak_ptr<VectorPool> m_pool;
    };

    struct Statistics
    {
        std::atomic<size_t> reuseCount{ 0 };
        std::atomic<size_t> allocationCount{ 0 };
        std::atomic<size_t> allocationDuration{ 0 };
    };

public:
    [[nodiscard]] static std::shared_ptr<VectorPool>
    create( const size_t vectorCapacity )
    {
        return std::shared_ptr<VectorPool>( new VectorPool( vectorCapacity ) );
    }

    /**
     * @todo try shared mutex, probably in combination with a second atomic<size_t> to get the next unused index
     * @todo try reinserting destructed containers at once without unlock and relocking for each buffer.
     */
    [[nodiscard]] WrappedContainer
    allocate()
    {
        const auto t0 = now();
        std::unique_lock lock{ m_mutex };
        if ( m_containers.empty() ) {
            lock.unlock();

            ++m_statistics.allocationCount;

            WrappedContainer result( Container(), this->shared_from_this() );
            result.reserve( m_vectorCapacity );
            m_statistics.allocationDuration += static_cast<size_t>( duration( t0 ) * 1e9 );
            return result;
        }

        auto result = std::move( m_containers.back() );
        m_containers.pop_back();
        lock.unlock();

        ++m_statistics.reuseCount;
        m_statistics.allocationDuration += static_cast<size_t>( duration( t0 ) * 1e9 );
        return WrappedContainer( std::move( result ), this->shared_from_this() );
    }

    [[nodiscard]] const Statistics&
    statistics() const
    {
        return m_statistics;
    }

private:
    explicit
    VectorPool( const size_t vectorCapacity ) :
        m_vectorCapacity( vectorCapacity )
    {}

    void
    reuse( Container&& container )
    {
        container.clear();  // This does not free memory, which is what we want!
        if ( container.capacity() == m_vectorCapacity ) {
            const std::scoped_lock lock{ m_mutex };
            m_containers.emplace_back( std::move( container ) );
        }
    }

private:
    const size_t m_vectorCapacity;

    Statistics m_statistics;

    std::mutex m_mutex;
    std::vector<Container> m_containers;
};
