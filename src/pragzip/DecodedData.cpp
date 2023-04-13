#include "DecodedData.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

#include <VectorView.hpp>

#include "DecodedDataView.hpp"
#include "definitions.hpp"
#include "MarkerReplacement.hpp"


namespace pragzip::deflate
{
void
DecodedData::append( DecodedDataView const& buffers )
{
    if ( buffers.dataWithMarkersSize() > 0 ) {
        if ( !data.empty() ) {
            throw std::invalid_argument( "It is not allowed to append data with markers when fully decoded data "
                                         "has already been appended because the ordering will be wrong!" );
        }

        auto& copied = dataWithMarkers.emplace_back();
        copied.reserve( buffers.dataWithMarkersSize() );
        for ( const auto& buffer : buffers.dataWithMarkers ) {
            copied.insert( copied.end(), buffer.begin(), buffer.end() );
        }
    }

    if ( buffers.dataSize() > 0 ) {
        auto& copied = data.emplace_back();
        copied.reserve( buffers.dataSize() );
        for ( const auto& buffer : buffers.data ) {
            copied.insert( copied.end(), buffer.begin(), buffer.end() );
        }
    }
}


void
DecodedData::applyWindow( WindowView const& window )
{
    const auto markerCount = dataWithMarkersSize();
    if ( markerCount == 0 ) {
        dataWithMarkers.clear();
        return;
    }

    /* Because of the overhead of copying the window, avoid it for small replacements. */
    if ( markerCount >= 128_Ki ) {
        const std::array<uint8_t, 64_Ki> fullWindow =
            [&window] () noexcept
            {
                std::array<uint8_t, 64_Ki> result{};
                std::iota( result.begin(), result.begin() + 256, 0 );
                std::copy( window.begin(), window.end(), result.begin() + MAX_WINDOW_SIZE );
                return result;
            }();

        DecodedVector downcasted( markerCount );
        size_t offset{ 0 };
        for ( auto& chunk : dataWithMarkers ) {
            std::transform( chunk.begin(), chunk.end(), downcasted.begin() + offset,
                            [&fullWindow] ( const auto value ) constexpr noexcept { return fullWindow[value]; } );
            offset += chunk.size();
        }

        data.insert( data.begin(), std::move( downcasted ) );
        dataWithMarkers.clear();
        return;
    }

    DecodedVector downcasted( markerCount );
    size_t offset{ 0 };

    /* For maximum size windows, we can skip one check because even UINT16_MAX is valid. */
    static_assert( std::numeric_limits<uint16_t>::max() - MAX_WINDOW_SIZE + 1U == MAX_WINDOW_SIZE );
    if ( window.size() >= MAX_WINDOW_SIZE ) {
        const MapMarkers<true> mapMarkers( window );
        for ( auto& chunk : dataWithMarkers ) {
            std::transform( chunk.begin(), chunk.end(), downcasted.begin() + offset, mapMarkers );
            offset += chunk.size();
        }
    } else {
        const MapMarkers<false> mapMarkers( window );
        for ( auto& chunk : dataWithMarkers ) {
            std::transform( chunk.begin(), chunk.end(), chunk.begin(), mapMarkers );
            std::copy( chunk.begin(), chunk.end(), reinterpret_cast<std::uint8_t*>( downcasted.data() + offset ) );
            offset += chunk.size();
        }
    }

    data.insert( data.begin(), std::move( downcasted ) );
    dataWithMarkers.clear();
}


[[nodiscard]] DecodedVector
DecodedData::getLastWindow( WindowView const& previousWindow ) const
{
    DecodedVector window( MAX_WINDOW_SIZE, 0 );
    size_t nBytesWritten{ 0 };

    /* Fill the result from the back with data from our buffer. */
    for ( auto chunk = data.rbegin(); ( chunk != data.rend() ) && ( nBytesWritten < window.size() ); ++chunk ) {
        for ( auto symbol = chunk->rbegin(); ( symbol != chunk->rend() ) && ( nBytesWritten < window.size() );
              ++symbol, ++nBytesWritten )
        {
            window[window.size() - 1 - nBytesWritten] = *symbol;
        }
    }

    /* Fill the result from the back with data from our unresolved buffers. */
    const auto copyFromDataWithMarkers =
        [this, &window, &nBytesWritten] ( const auto& mapMarker )
        {
            for ( auto chunk = dataWithMarkers.rbegin();
                  ( chunk != dataWithMarkers.rend() ) && ( nBytesWritten < window.size() ); ++chunk )
            {
                for ( auto symbol = chunk->rbegin(); ( symbol != chunk->rend() ) && ( nBytesWritten < window.size() );
                      ++symbol, ++nBytesWritten )
                {
                    window[window.size() - 1 - nBytesWritten] = mapMarker( *symbol );
                }
            }
        };

    if ( previousWindow.size() >= MAX_WINDOW_SIZE ) {
        copyFromDataWithMarkers( MapMarkers</* full window */ true>( previousWindow ) );
    } else {
        copyFromDataWithMarkers( MapMarkers</* full window */ false>( previousWindow ) );
    }

    /* Fill the remaining part with the given window. This should only happen for very small DecodedData sizes. */
    if ( nBytesWritten < MAX_WINDOW_SIZE ) {
        const auto remainingBytes = MAX_WINDOW_SIZE - nBytesWritten;
        std::copy( std::reverse_iterator( previousWindow.end() ),
                   std::reverse_iterator( previousWindow.end() )
                   + std::min( remainingBytes, previousWindow.size() ),
                   window.rbegin() + nBytesWritten );
    }

    return window;
}


[[nodiscard]] DecodedVector
DecodedData::getWindowAt( WindowView const& previousWindow,
                          size_t const      skipBytes) const
{
    if ( skipBytes > size() ) {
        throw std::invalid_argument( "Amount of bytes to skip is larger than this block!" );
    }

    DecodedVector window( MAX_WINDOW_SIZE );
    size_t prefilled{ 0 };
    if ( skipBytes < MAX_WINDOW_SIZE ) {
        const auto lastBytesToCopyFromPrevious = MAX_WINDOW_SIZE - skipBytes;
        if ( lastBytesToCopyFromPrevious <= previousWindow.size() ) {
            for ( size_t j = previousWindow.size() - lastBytesToCopyFromPrevious; j < previousWindow.size();
                  ++j, ++prefilled )
            {
                window[prefilled] = previousWindow[j];
            }
            // prefilled = lastBytesToCopyFromPrevious = MAX_WINDOW_SIZE - skipBytes
        } else {
            /* If previousWindow.size() < MAX_WINDOW_SIZE, which might happen at the start of streams,
             * then behave as if previousWindow was padded with leading zeros. */
            const auto zerosToFill = lastBytesToCopyFromPrevious - previousWindow.size();
            for ( ; prefilled < zerosToFill; ++prefilled ) {
                window[prefilled] = 0;
            }

            for ( size_t j = 0; j < previousWindow.size(); ++j, ++prefilled ) {
                window[prefilled] = previousWindow[j];
            }
            // prefilled = lastBytesToCopyFromPrevious - previousWindow.size() + previousWindow.size()
        }
        assert( prefilled == MAX_WINDOW_SIZE - skipBytes );
    }

    const auto remainingBytes = window.size() - prefilled;

    /* Skip over skipBytes in data and then copy the last remainingBytes before it. */
    auto offset = skipBytes - remainingBytes;
    /* if skipBytes < MAX_WINDOW_SIZE
     *     offset = skipBytes - ( window.size() - ( MAX_WINDOW_SIZE - skipBytes ) ) = 0
     * if skipBytes >= MAX_WINDOW_SIZE
     *     offset = skipBytes - ( window.size() - 0 ) */

    const auto copyFromDataWithMarkers =
        [this, &offset, &prefilled, &window] ( const auto& mapMarker )
        {
            for ( auto& chunk : dataWithMarkers ) {
                if ( prefilled >= window.size() ) {
                    break;
                }

                if ( offset >= chunk.size() ) {
                    offset -= chunk.size();
                    continue;
                }

                for ( size_t i = offset; ( i < chunk.size() ) && ( prefilled < window.size() ); ++i, ++prefilled ) {
                    window[prefilled] = mapMarker( chunk[i] );
                }
                offset = 0;
            }
        };

    if ( previousWindow.size() >= MAX_WINDOW_SIZE ) {
        copyFromDataWithMarkers( MapMarkers</* full window */ true>( previousWindow ) );
    } else {
        copyFromDataWithMarkers( MapMarkers</* full window */ false>( previousWindow ) );
    }

    for ( auto& chunk : data ) {
        if ( prefilled >= window.size() ) {
            break;
        }

        if ( offset >= chunk.size() ) {
            offset -= chunk.size();
            continue;
        }

        for ( size_t i = offset; ( i < chunk.size() ) && ( prefilled < window.size() ); ++i, ++prefilled ) {
            window[prefilled] = chunk[i];
        }
        offset = 0;
    }

    return window;
}


void
DecodedData::cleanUnmarkedData()
{
    while ( !dataWithMarkers.empty() ) {
        const auto& toDowncast = dataWithMarkers.back();
        /* Try to not only downcast whole chunks of data but also as many bytes as possible for the last chunk. */
        const auto marker = std::find_if(
            toDowncast.rbegin(), toDowncast.rend(),
            [] ( auto value ) { return value > std::numeric_limits<uint8_t>::max(); } );

        const auto sizeWithoutMarkers = static_cast<size_t>( std::distance( toDowncast.rbegin(), marker ) );
        auto downcasted = data.emplace( data.begin(), sizeWithoutMarkers );
        std::transform( marker.base(), toDowncast.end(), downcasted->begin(),
                        [] ( auto symbol ) { return static_cast<uint8_t>( symbol ); } );

        if ( marker == toDowncast.rend() ) {
            dataWithMarkers.pop_back();
        } else {
            dataWithMarkers.back().resize( dataWithMarkers.back().size() - sizeWithoutMarkers );
            break;
        }
    }

    shrinkToFit();
}


/**
 * m pragzip && src/tools/pragzip -v -d -c -P 0 4GiB-base64.gz | wc -c
 * Non-polymorphic: Decompressed in total 4294967296 B in 1.49444 s -> 2873.96 MB/s
 * With virtual ~DecodedData() = default: Decompressed in total 4294967296 B in 3.58325 s -> 1198.62 MB/s
 * I don't know why it happens. Maybe it affects inline of function calls or moves of instances.
 */
static_assert( !std::is_polymorphic_v<DecodedData>, "Simply making it polymorphic halves performance!" );
}  // namespace pragzip::deflate
