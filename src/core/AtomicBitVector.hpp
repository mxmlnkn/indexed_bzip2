#pragma once

#include <atomic>
#include <limits>
#include <stdexcept>
#include <vector>

#include <common.hpp>


class AtomicBitVector
{
private:
    using ChunkType = uint64_t;

public:
    AtomicBitVector( size_t size ) :
        m_size( size ),
        m_data( ceilDiv( size, std::numeric_limits<ChunkType>::digits ), 0 )
    {}

    [[nodiscard]] size_t
    size() const noexcept
    {
        return m_size;
    }

    void
    set( size_t i )
    {
        if ( i >= m_size ) {
            throw std::out_of_range( "Index out of range" );
        }

        //m_usedChunks[] |= uint64_t( 1 ) << ;
    }

    [[nodiscard]] bool
    get() const noexcept
    {
        //return m_usedChunks[];
        return false;
    }

private:
    const size_t m_size;
    std::vector<std::atomic<ChunkType> > m_data;
};
