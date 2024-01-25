#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>


#if 0
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

        ~WrappedContainer()
        {
            if ( const auto sharedPool = m_pool.lock() ) {
                sharedPool->reuse( std::move( static_cast<Container&>( *this ) ) );
            }
        }

    private:
        const std::weak_ptr<VectorPool> m_pool;
    };

    struct Statistics
    {
        std::atomic<size_t> reuseCount;
        std::atomic<size_t> allocationCount;
    };

public:
    [[nodiscard]] static std::shared_ptr<VectorPool>
    create( const size_t vectorCapacity )
    {
        return std::shared_ptr<VectorPool>( new VectorPool( vectorCapacity ) );
    }

    [[nodiscard]] WrappedContainer
    allocate()
    {
        std::unique_lock lock{ m_mutex };
        if ( m_containers.empty() ) {
            lock.unlock();

            ++m_statistics.allocationCount;

            WrappedContainer result( Container(), this->shared_from_this() );
            result.reserve( m_vectorCapacity );
            return result;
        }

        ++m_statistics.reuseCount;
        auto result = std::move( m_containers.back() );
        m_containers.pop_back();
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
#else

template<typename Container>
class VectorPool :
    public std::enable_shared_from_this<VectorPool<Container> >
{
public:
    /**
     * These containers are only created via VectorPool::create anyway, so we don't have to worry
     * about having to redeclare and redirect to all possible std::vector constructors when inheriting.
     */
    /*class WrappedContainer :
        public Container
    {
    public:
        WrappedContainer() = default;

        //explicit
        //WrappedContainer( std::weak_ptr<VectorPool> pool ) :
        //    m_pool( std::move( pool ) )
        //{}
        //
        //WrappedContainer( Container&&               container,
        //                  std::weak_ptr<VectorPool> pool ) :
        //    Container( std::move( container ) ),
        //    m_pool( std::move( pool ) )
        //{}

        ~WrappedContainer()
        {
            //if ( const auto sharedPool = m_pool.lock() ) {
                //sharedPool->reuse( std::move( static_cast<Container&>( *this ) ) );
            //}
        }

    private:
        //const std::weak_ptr<VectorPool> m_pool;
    };*/
    using WrappedContainer = Container;

    struct Statistics
    {
        std::atomic<size_t> reuseCount;
        std::atomic<size_t> allocationCount;
    };

public:
    [[nodiscard]] static std::shared_ptr<VectorPool>
    create( const size_t vectorCapacity )
    {
        return std::shared_ptr<VectorPool>( new VectorPool( vectorCapacity ) );
    }

    [[nodiscard]] WrappedContainer
    allocate()
    {
        WrappedContainer result; //( this->weak_from_this() );
        result.reserve( m_vectorCapacity );
        return result;
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

private:
    const size_t m_vectorCapacity;
    Statistics m_statistics;

    //std::mutex m_mutex;
    //std::vector<Container> m_containers;
};
#endif
