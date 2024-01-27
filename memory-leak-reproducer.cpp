/*
g++ -std=c++17 ../memory-leak-reproducer.cpp && ./a.out
*/

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>


class VectorPool
{
public:
    class WrappedContainer
    {
    public:
        WrappedContainer() = default;
        /* This destructor definition leads to a memory leak resulting in an OOM kill after 70 GB! */
        ~WrappedContainer() {}

        [[nodiscard]] auto capacity() const { return m_container.capacity(); };
        [[nodiscard]] auto size() const { return m_container.size(); };
        void reserve( size_t newCapacity ) { return m_container.reserve( newCapacity ); };
    private:
        std::vector<uint16_t> m_container;
    };

    [[nodiscard]] static WrappedContainer
    allocate()
    {
        WrappedContainer result;
        result.reserve( 1024 );
        return result;
    }
};


struct DecodedData
{
public:
    void
    append( const size_t toAppendSize )
    {
        if ( dataWithMarkers.empty() ) {
            dataWithMarkers.emplace_back( VectorPool::allocate() );
        }

        for ( size_t nCopied = 0; nCopied < toAppendSize; ) {
            auto& copyTarget = dataWithMarkers.back();
            const auto nFreeElements = copyTarget.capacity() - copyTarget.size();
            if ( nFreeElements == 0 ) {
                dataWithMarkers.emplace_back( VectorPool::allocate() );
                continue;
            }

            const auto nToCopy = std::min( nFreeElements, toAppendSize - nCopied );
            nCopied += nToCopy;
        }
    }

private:
    std::vector<VectorPool::WrappedContainer> dataWithMarkers;
};


int
main()
{
    DecodedData decodedData;
    decodedData.append( 1 );
    return 0;
}
