#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

#include <FasterVector.hpp>
#include <VectorView.hpp>

#include "DecodedDataView.hpp"
#include "definitions.hpp"


namespace pragzip::deflate
{
using MarkerVector = FasterVector<uint16_t>;
using DecodedVector = FasterVector<uint8_t>;


struct DecodedData
{
public:
    using WindowView = VectorView<uint8_t>;

    class Iterator
    {
    public:
        explicit
        Iterator( const DecodedData& decodedData,
                  const size_t       offset = 0,
                  const size_t       size = std::numeric_limits<size_t>::max() ) :
            m_data( decodedData ),
            m_size( size )
        {
            m_offsetInChunk = offset;
            for ( m_currentChunk = 0; m_currentChunk < m_data.data.size(); ++m_currentChunk ) {
                const auto& chunk = m_data.data[m_currentChunk];
                if ( ( m_offsetInChunk < chunk.size() ) && !chunk.empty() ) {
                    m_sizeInChunk = std::min( chunk.size() - m_offsetInChunk, m_size );
                    break;
                }
                m_offsetInChunk -= chunk.size();
            }
        }

        [[nodiscard]] operator bool() const noexcept
        {
            return ( m_currentChunk < m_data.data.size() ) && ( m_processedSize < m_size );
        }

        [[nodiscard]] std::pair<const void*, uint64_t>
        operator*() const
        {
            const auto& chunk = m_data.data[m_currentChunk];
            return { chunk.data() + m_offsetInChunk, m_sizeInChunk };
        }

        void
        operator++()
        {
            m_processedSize += m_sizeInChunk;
            m_offsetInChunk = 0;
            m_sizeInChunk = 0;

            if ( m_processedSize > m_size ) {
                throw std::logic_error( "Iterated over more bytes than was requested!" );
            }

            if ( !static_cast<bool>( *this ) ) {
                return;
            }

            ++m_currentChunk;
            for ( ; m_currentChunk < m_data.data.size(); ++m_currentChunk ) {
                const auto& chunk = m_data.data[m_currentChunk];
                if ( !chunk.empty() ) {
                    m_sizeInChunk = std::min( chunk.size(), m_size - m_processedSize );
                    break;
                }
            }
        }

    private:
        const DecodedData& m_data;
        const size_t m_size;

        size_t m_currentChunk{ 0 };
        size_t m_offsetInChunk{ 0 };
        size_t m_sizeInChunk{ 0 };
        size_t m_processedSize{ 0 };
    };

public:
    void
    append( DecodedVector&& toAppend )
    {
        if ( !toAppend.empty() ) {
            data.emplace_back( std::move( toAppend ) );
            data.back().shrink_to_fit();
        }
    }

    void
    append( DecodedDataView const& buffers );

    [[nodiscard]] size_t
    dataSize() const noexcept
    {
        const auto addSize = [] ( const size_t size, const auto& container ) { return size + container.size(); };
        return std::accumulate( data.begin(), data.end(), size_t( 0 ), addSize );
    }

    [[nodiscard]] size_t
    dataWithMarkersSize() const noexcept
    {
        const auto addSize = [] ( const size_t size, const auto& container ) { return size + container.size(); };
        return std::accumulate( dataWithMarkers.begin(), dataWithMarkers.end(), size_t( 0 ), addSize );
    }

    [[nodiscard]] size_t
    size() const noexcept
    {
        return dataSize() + dataWithMarkersSize();
    }

    [[nodiscard]] size_t
    sizeInBytes() const noexcept
    {
        return dataSize() * sizeof( uint8_t ) + dataWithMarkersSize() * sizeof( uint16_t );
    }

    /**
     * This is used to determine whether it is necessary to call applyWindow.
     * Testing for @ref dataWithMarkers.empty() is not sufficient because markers could be contained
     * in other members for derived classes! In that case @ref containsMarkers will be overriden.
     * @note Probably should not be called internally because it is allowed to be shadowed by a child class method.
     */
    [[nodiscard]] bool
    containsMarkers() const noexcept
    {
        return !dataWithMarkers.empty();
    }

    /**
     * Replaces all 16-bit wide marker symbols by looking up the referenced 8-bit symbols in @p window.
     * @note Probably should not be called internally because it is allowed to be shadowed by a child class method.
     */
    void
    applyWindow( WindowView const& window );

    /**
     * Returns the last 32 KiB decoded bytes. This can be called after decoding a block has finished
     * and then can be used to store and load it with deflate::Block::setInitialWindow to restart decoding
     * with the next block. Because this is not supposed to be called very often, it returns a copy of
     * the data instead of views.
     */
    [[nodiscard]] DecodedVector
    getLastWindow( WindowView const& previousWindow ) const;

    /**
     * @param skipBytes The number of bits to shift the previous window and fill it with new data.
     *        A value of 0 would simply return @p previousWindow while a value equal to size() would return
     *        the window as it would be after this whole block.
     * @note Should only be called after @ref applyWindow because @p skipBytes larger than @ref dataSize will throw.
     * @note Probably should not be called internally because it is allowed to be shadowed by a child class method.
     */
    [[nodiscard]] DecodedVector
    getWindowAt( WindowView const& previousWindow,
                 size_t            skipBytes ) const;

    void
    shrinkToFit()
    {
        for ( auto& container : data ) {
            container.shrink_to_fit();
        }
        for ( auto& container : dataWithMarkers ) {
            container.shrink_to_fit();
        }
    }

    /**
     * Check decoded blocks that account for possible markers whether they actually contain markers and, if not so,
     * convert and move them to actual decoded data.
     */
    void
    cleanUnmarkedData();

public:
    size_t encodedOffsetInBits{ std::numeric_limits<size_t>::max() };
    size_t encodedSizeInBits{ 0 };

    /**
     * Use vectors of vectors to avoid reallocations. The order of this data is:
     * - @ref dataWithMarkers (front to back)
     * - @ref data (front to back)
     * This order is fixed because there should be no reason for markers after we got enough data without markers!
     * There is no append( DecodedData ) method because this property might not be retained after using
     * @ref cleanUnmarkedData.
     */
    std::vector<MarkerVector> dataWithMarkers;
    std::vector<DecodedVector> data;
};


#ifdef HAVE_IOVEC
[[nodiscard]] inline std::vector<::iovec>
toIoVec( const DecodedData& decodedData,
         const size_t       offsetInBlock,
         const size_t       dataToWriteSize )
{
    std::vector<::iovec> buffersToWrite;
    for ( auto it = pragzip::deflate::DecodedData::Iterator( decodedData, offsetInBlock, dataToWriteSize );
          static_cast<bool>( it ); ++it )
    {
        const auto& [data, size] = *it;
        ::iovec buffer;
        /* The const_cast should be safe because vmsplice and writev should not modify the input data. */
        buffer.iov_base = const_cast<void*>( reinterpret_cast<const void*>( data ) );;
        buffer.iov_len = size;
        buffersToWrite.emplace_back( buffer );
    }
    return buffersToWrite;
}
#endif  // HAVE_IOVEC
}  // namespace pragzip::deflate
