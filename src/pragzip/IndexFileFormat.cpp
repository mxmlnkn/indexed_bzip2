#include "IndexFileFormat.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include <common.hpp>
#include <filereader/FileReader.hpp>
#include <FileUtils.hpp>


[[nodiscard]] GzipIndex
readGzipIndex( UniqueFileReader file )
{
    GzipIndex index;

    const auto checkedRead =
        [&file] ( void* buffer, size_t size )
        {
            const auto nBytesRead = file->read( reinterpret_cast<char*>( buffer ), size );
            if ( nBytesRead != size ) {
                throw std::runtime_error( "Premature end of index file! Got only " + std::to_string( nBytesRead )
                                          + " out of " + std::to_string( size ) + " requested bytes." );
            }
        };

    const auto loadValue = [&checkedRead] ( auto& destination ) { checkedRead( &destination, sizeof( destination ) ); };

    std::vector<char> formatId( 5, 0 );
    checkedRead( formatId.data(), formatId.size() );
    if ( formatId != std::vector<char>( { 'G', 'Z', 'I', 'D', 'X' } ) ) {
        throw std::invalid_argument( "Invalid magic bytes!" );
    }

    const auto formatVersion = readValue<uint8_t>( file.get() );
    if ( formatVersion > 1 ) {
        throw std::invalid_argument( "Index was written with a newer indexed_gzip version than supported!" );
    }

    file->seek( 1, SEEK_CUR );  // Skip reserved flags 1B

    loadValue( index.compressedSizeInBytes );
    loadValue( index.uncompressedSizeInBytes );
    loadValue( index.checkpointSpacing );
    loadValue( index.windowSizeInBytes );

    /* However, a window size larger than 32 KiB makes no sense bacause the Lempel-Ziv back-references
     * in the deflate format are limited to 32 KiB! Smaller values might, however, be enforced by especially
     * memory-constrained encoders.
     * This basically means that we either check for this to be exactly 32 KiB or we simply throw away all
     * other data and only load the last 32 KiB of the window buffer. */
    if ( index.windowSizeInBytes != 32_Ki ) {
        throw std::invalid_argument( "Only a window size of 32 KiB makes sense because indexed_gzip supports "
                                     "no smaller ones and gzip does supprt any larger one." );
    }
    const auto checkpointCount = readValue<uint32_t>( file.get() );

    index.checkpoints.resize( checkpointCount );
    for ( uint32_t i = 0; i < checkpointCount; ++i ) {
        auto& checkpoint = index.checkpoints[i];

        /* First load only compressed offset rounded down in bytes, the bits are loaded down below! */
        loadValue( checkpoint.compressedOffsetInBits );
        if ( checkpoint.compressedOffsetInBits > index.compressedSizeInBytes ) {
            throw std::invalid_argument( "Checkpoint compressed offset is after the file end!" );
        }
        checkpoint.compressedOffsetInBits *= 8;

        loadValue( checkpoint.uncompressedOffsetInBytes );
        if ( checkpoint.uncompressedOffsetInBytes > index.uncompressedSizeInBytes ) {
            throw std::invalid_argument( "Checkpoint uncompressed offset is after the file end!" );
        }

        const auto bits = readValue<uint8_t>( file.get() );
        if ( bits >= 8 ) {
            throw std::invalid_argument( "Denormal compressed offset for checkpoint. Bit offset >= 8!" );
        }
        if ( bits > 0 ) {
            if ( checkpoint.compressedOffsetInBits == 0 ) {
                throw std::invalid_argument( "Denormal bits for checkpoint. Effectively negative offset!" );
            }
            checkpoint.compressedOffsetInBits -= bits;
        }

        if ( formatVersion == 0 ) {
            if ( i != 0 ) {
                checkpoint.window.resize( index.windowSizeInBytes );
            }
        } else {
            if ( /* data flag */ readValue<uint8_t>( file.get() ) != 0 ) {
                checkpoint.window.resize( index.windowSizeInBytes );
            }
        }
    }

    for ( auto& checkpoint : index.checkpoints ) {
        if ( !checkpoint.window.empty() ) {
            checkedRead( checkpoint.window.data(), checkpoint.window.size() );
        }
    }

    return index;
}


void
writeGzipIndex( const GzipIndex&                                              index,
                const std::function<void( const void* buffer, size_t size )>& checkedWrite )
{
    const auto writeValue = [&checkedWrite] ( auto value ) { checkedWrite( &value, sizeof( value ) ); };

    const auto& checkpoints = index.checkpoints;
    const uint32_t windowSizeInBytes = static_cast<uint32_t>( 32_Ki );

    if ( !std::all_of( checkpoints.begin(), checkpoints.end(), [windowSizeInBytes] ( const auto& checkpoint ) {
        return checkpoint.window.empty() || ( checkpoint.window.size() >= windowSizeInBytes ); } ) )
    {
        throw std::invalid_argument( "All window sizes must be at least 32 KiB!" );
    }

    checkedWrite( "GZIDX", 5 );
    checkedWrite( /* format version */ "\x01", 1 );
    checkedWrite( /* reserved flags */ "\x00", 1 );

    /* The spacing is only used for decompression, so after reading a >full< index file, it should be irrelevant! */
    uint32_t checkpointSpacing = index.checkpointSpacing;

    if ( !checkpoints.empty() && ( checkpointSpacing < windowSizeInBytes ) ) {
        std::vector<uint64_t> uncompressedOffsets( checkpoints.size() );
        std::transform( checkpoints.begin(), checkpoints.end(), uncompressedOffsets.begin(),
                        [] ( const auto& checkpoint ) { return checkpoint.uncompressedOffsetInBytes; } );
        std::adjacent_difference( uncompressedOffsets.begin(), uncompressedOffsets.end(), uncompressedOffsets.begin() );
        const auto minSpacing = std::accumulate( uncompressedOffsets.begin() + 1, uncompressedOffsets.end(),
                                                 uint64_t( 0 ), [] ( auto a, auto b ) { return std::min( a, b ); } );
        checkpointSpacing = std::max( windowSizeInBytes, static_cast<uint32_t>( minSpacing ) );
    }

    writeValue( index.compressedSizeInBytes );
    writeValue( index.uncompressedSizeInBytes );
    writeValue( checkpointSpacing );
    writeValue( windowSizeInBytes );
    writeValue( static_cast<uint32_t>( checkpoints.size() ) );

    for ( const auto& checkpoint : checkpoints ) {
        const auto bits = checkpoint.compressedOffsetInBits % 8;
        writeValue( checkpoint.compressedOffsetInBits / 8 + ( bits == 0 ? 0 : 1 ) );
        writeValue( checkpoint.uncompressedOffsetInBytes );
        writeValue( static_cast<uint8_t>( bits == 0 ? 0 : 8 - bits ) );
        writeValue( static_cast<uint8_t>( checkpoint.window.empty() ? 0 : 1 ) );
    }

    for ( const auto& checkpoint : checkpoints ) {
        const auto& window = checkpoint.window;
        if ( window.empty() ) {
            continue;
        }

        if ( window.size() == windowSizeInBytes ) {
            checkedWrite( window.data(), window.size() );
        } else if ( window.size() > windowSizeInBytes ) {
            checkedWrite( window.data() + window.size() - windowSizeInBytes, windowSizeInBytes );
        } else if ( window.size() < windowSizeInBytes ) {
            const std::vector<char> zeros( windowSizeInBytes - window.size(), 0 );
            checkedWrite( zeros.data(), zeros.size() );
            checkedWrite( window.data(), window.size() );
        }
    }
}
