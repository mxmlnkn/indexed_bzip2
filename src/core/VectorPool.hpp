#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>


template<typename Container>
class VectorPool :
    public std::enable_shared_from_this<VectorPool<Container> >
{
public:
    class WrappedContainer
    {
    public:
        using const_iterator = typename Container::const_iterator;

    public:
        WrappedContainer() = default;
        /* This destructor definition leads to a memory leak resulting in an OOM kill after 70 GB! */
        ~WrappedContainer() {}

        [[nodiscard]] auto data() { return m_container.data(); };
        [[nodiscard]] auto data() const { return m_container.data(); };
        [[nodiscard]] auto begin() const { return m_container.begin(); };
        [[nodiscard]] auto end() const { return m_container.end(); };
        [[nodiscard]] auto rbegin() const { return m_container.rbegin(); };
        [[nodiscard]] auto rend() const { return m_container.rend(); };
        [[nodiscard]] auto capacity() const { return m_container.capacity(); };
        [[nodiscard]] auto size() const { return m_container.size(); };
        void reserve( size_t newCapacity ) { return m_container.reserve( newCapacity ); };
        void resize( size_t newSize ) { return m_container.resize( newSize ); };
        void shrink_to_fit() { return m_container.shrink_to_fit(); };
        template<class InputIt>
        auto insert( const_iterator pos, InputIt first, InputIt last ) { return m_container.insert( pos, first, last ); };
        [[nodiscard]] auto operator[]( size_t index ) const { return m_container.operator[]( index ); };
    private:
        Container m_container;
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

    std::mutex m_mutex;
    std::vector<Container> m_containers;
};
