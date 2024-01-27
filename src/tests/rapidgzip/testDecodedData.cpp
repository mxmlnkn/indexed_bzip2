/*
g++ -std=c++17 ../memory-leak-reproducer.cpp && ./a.out
*/

#include <cstdint>
#include <iostream>
#include <vector>

template<typename WrappedContainer>
class VectorPool
{
public:
    [[nodiscard]] static WrappedContainer
    allocate()
    {
        WrappedContainer result;
        result.reserve( 1024 );
        return result;
    }
};

class WrappedContainerWithoutDestructor
{
public:
    WrappedContainerWithoutDestructor() = default;
    [[nodiscard]] auto capacity() const { return m_container.capacity(); };
    void reserve( size_t newCapacity ) { return m_container.reserve( newCapacity ); };
private:
    std::vector<uint16_t> m_container;
};

class WrappedContainerWithDestructor
{
public:
    WrappedContainerWithDestructor() = default;
    ~WrappedContainerWithDestructor() = default;
    [[nodiscard]] auto capacity() const { return m_container.capacity(); };
    void reserve( size_t newCapacity ) { return m_container.reserve( newCapacity ); };
private:
    std::vector<uint16_t> m_container;
};

using VectorPoolWithDestructor = VectorPool<WrappedContainerWithDestructor>;
using VectorPoolWithoutDestructor = VectorPool<WrappedContainerWithoutDestructor>;

int
main()
{
    std::cerr << "Capacity of returned vector when WrappedContainer has NO destructor: "
              << VectorPoolWithoutDestructor::allocate().capacity() << "\n";
    std::cerr << "Capacity of returned vector when WrappedContainer has a destructor: "
              << VectorPoolWithDestructor::allocate().capacity() << "\n";

    std::vector<WrappedContainerWithoutDestructor> dataWithoutDestructor;
    dataWithoutDestructor.emplace_back( VectorPoolWithoutDestructor::allocate() );
    std::cerr << "Capacity of emplaced vector when WrappedContainer has NO destructor: "
              << dataWithoutDestructor.back().capacity() << "\n";

    std::vector<WrappedContainerWithDestructor> dataWithDestructor;
    dataWithDestructor.emplace_back( VectorPoolWithDestructor::allocate() );
    std::cerr << "Capacity of emplaced vector when WrappedContainer has a destructor: "
              << dataWithDestructor.back().capacity() << "\n";
    return 0;
}
