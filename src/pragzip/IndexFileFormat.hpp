#pragma once

#include <functional>
#include <stdexcept>
#include <vector>

#include <common.hpp>                   // _Ki literals
#include <filereader/FileReader.hpp>


/**
 * File Format:
 * @see zran_export_index and zran_import_index functions in indexed_gzip https://github.com/pauldmccarthy/indexed_gzip
 *
 * @verbatim
 * 00  GZIDX      # Index File ID
 * 05  \x01       # File Version
 * 06  \x00       # Flags (Unused)
 * 07  <8B>       # Compressed Size (uint64_t)
 * 15  <8B>       # Uncompressed Size (uint64_t)
 * 23  <4B>       # Spacing (uint32_t)
 * 27  <4B>       # Window Size (uint32_t), Expected to be 32768, indexed_gzip checks that it is >= 32768.
 * 31  <4B>       # Number of Checkpoints (uint32_t)
 * 35
 * <Checkpoint Data> (Repeated Number of Checkpoints Times)
 * > 00  <8B>       # Compressed Offset in Rounded Down Bytes (uint64_t)
 * > 08  <8B>       # Uncompressed Offset (uint64_t)
 * > 16  <1B>       # Bits (uint8_t), Possible Values: 0-7
 * >                # "this is the number of bits in the compressed data, before the [byte offset]"
 * > 17  <1B>       # Data Flag (uint8_t), 1 if this checkpoint has window data, else 0.
 * > 18             # For format version 0, this flag did not exist and all but the first checkpoint had windows!
 * <Window Data> (Might be fewer than checkpoints because no data is written for e.g. stream boundaries)
 * > 00  <Window Size Bytes>  # Window Data, i.e., uncompressed buffer before the checkpoint's offset.
 * @endverbatim
 *
 * @note The checkpoint and window data have fixed length, so theoretically, the data could be read
 *       on-demand from the file by seeking to the required position.
 */


struct Checkpoint
{
    uint64_t compressedOffsetInBits{ 0 };
    uint64_t uncompressedOffsetInBytes{ 0 };
    /** The window may be empty for the first deflate block in each gzip stream. */
    std::vector<uint8_t> window;

    [[nodiscard]] constexpr bool
    operator==( const Checkpoint& other ) const noexcept
    {
        // *INDENT-OFF*
        return ( compressedOffsetInBits    == other.compressedOffsetInBits    ) &&
               ( uncompressedOffsetInBytes == other.uncompressedOffsetInBytes ) &&
               ( window                    == other.window                    );
        // *INDENT-ON*
    }
};


struct GzipIndex
{
    uint64_t compressedSizeInBytes{ 0 };
    uint64_t uncompressedSizeInBytes{ 0 };
    /**
     * This is a kind of guidance for spacing between checkpoints in the uncompressed data!
     * If the compression ratio is very high, it could mean that the checkpoint sizes can be larger
     * than the compressed file even for very large spacings.
     */
    uint32_t checkpointSpacing{ 0 };
    uint32_t windowSizeInBytes{ 0 };
    std::vector<Checkpoint> checkpoints;

    [[nodiscard]] constexpr bool
    operator==( const GzipIndex& other ) const noexcept
    {
        // *INDENT-OFF*
        return ( compressedSizeInBytes   == other.compressedSizeInBytes   ) &&
               ( uncompressedSizeInBytes == other.uncompressedSizeInBytes ) &&
               ( checkpointSpacing       == other.checkpointSpacing       ) &&
               ( windowSizeInBytes       == other.windowSizeInBytes       ) &&
               ( checkpoints             == other.checkpoints             );
        // *INDENT-ON*
    }
};


template<typename T>
[[nodiscard]] T
readValue( FileReader* file )
{
    /* Note that indexed_gzip itself does no endiannes check or conversion during writing,
     * so this system-specific reading is as portable as it gets assuming that the indexes are
     * read on the same system they are written. */
    T value;
    if ( file->read( reinterpret_cast<char*>( &value ), sizeof( value ) ) != sizeof( value ) ) {
        throw std::invalid_argument( "Premature end of file!" );
    }
    return value;
}


[[nodiscard]] GzipIndex
readGzipIndex( UniqueFileReader file );


void
writeGzipIndex( const GzipIndex&                                              index,
                const std::function<void( const void* buffer, size_t size )>& checkedWrite );
