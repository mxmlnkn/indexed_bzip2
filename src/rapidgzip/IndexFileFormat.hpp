#pragma once

#include <algorithm>
#include <cstdio>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <blockfinder/Bgzf.hpp>
#include <common.hpp>                   // _Ki literals
#include <filereader/FileReader.hpp>
#include <FileUtils.hpp>
#ifdef WITH_ISAL
    #include <isal.hpp>
#endif
#include <VectorView.hpp>
#include <zlib.hpp>

#include "WindowMap.hpp"


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

    [[nodiscard]] constexpr bool
    operator==( const Checkpoint& other ) const noexcept
    {
        return ( compressedOffsetInBits == other.compressedOffsetInBits ) &&
               ( uncompressedOffsetInBytes == other.uncompressedOffsetInBytes );
    }
};


struct GzipIndex
{
public:
    GzipIndex() = default;
    GzipIndex( GzipIndex&& ) = default;
    GzipIndex& operator=( GzipIndex&& ) = default;

    [[nodiscard]] GzipIndex
    clone() const
    {
        GzipIndex result( *this );
        if ( windows ) {
            result.windows = std::make_shared<WindowMap>( *windows );
        }
        return result;
    }

private:
    /* Forbid copies because it is unexpected that the windows are shared between copies! */
    GzipIndex( const GzipIndex& ) = default;
    GzipIndex& operator=( const GzipIndex& ) = default;

public:
    uint64_t compressedSizeInBytes{ std::numeric_limits<uint64_t>::max() };
    uint64_t uncompressedSizeInBytes{ std::numeric_limits<uint64_t>::max() };
    /**
     * This is a kind of guidance for spacing between checkpoints in the uncompressed data!
     * If the compression ratio is very high, it could mean that the checkpoint sizes can be larger
     * than the compressed file even for very large spacings.
     */
    uint32_t checkpointSpacing{ 0 };
    uint32_t windowSizeInBytes{ 0 };
    std::vector<Checkpoint> checkpoints;

    std::shared_ptr<WindowMap> windows;

    [[nodiscard]] constexpr bool
    operator==( const GzipIndex& other ) const noexcept
    {
        // *INDENT-OFF*
        return ( compressedSizeInBytes   == other.compressedSizeInBytes   ) &&
               ( uncompressedSizeInBytes == other.uncompressedSizeInBytes ) &&
               ( checkpointSpacing       == other.checkpointSpacing       ) &&
               ( windowSizeInBytes       == other.windowSizeInBytes       ) &&
               ( checkpoints             == other.checkpoints             ) &&
               ( ( windows == other.windows ) || ( windows && other.windows && ( *windows == *other.windows ) ) );
        // *INDENT-ON*
    }
};


std::ostream&
operator<<( std::ostream&    out,
            const GzipIndex& index )
{
    out << "GzipIndex{\n";
    out << "  compressedSizeInBytes: " << index.compressedSizeInBytes << "\n";
    out << "  uncompressedSizeInBytes: " << index.uncompressedSizeInBytes << "\n";
    out << "  checkpointSpacing: " << index.checkpointSpacing << "\n";
    out << "  windowSizeInBytes: " << index.windowSizeInBytes << "\n";
    out << "  checkpoints: {\n    ";
    for ( const auto& checkpoint : index.checkpoints ) {
        out << checkpoint.compressedOffsetInBits << ":" << checkpoint.uncompressedOffsetInBytes << ", ";
    }
    out << "  }\n}\n";
    return out;
}


void
checkedRead( FileReader* const indexFile,
             void*             buffer,
             size_t            size )
{
    if ( indexFile == nullptr ) {
        throw std::invalid_argument( "Index file reader must be valid!" );
    }
    const auto nBytesRead = indexFile->read( reinterpret_cast<char*>( buffer ), size );
    if ( nBytesRead != size ) {
        throw std::runtime_error( "Premature end of index file! Got only " + std::to_string( nBytesRead )
                                  + " out of " + std::to_string( size ) + " requested bytes." );
    }
}


template<typename T>
[[nodiscard]] T
readValue( FileReader* const file )
{
    /* Note that indexed_gzip itself does no endiannes check or conversion during writing,
     * so this system-specific reading is as portable as it gets assuming that the indexes are
     * read on the same system they are written. */
    T value;
    checkedRead( file, &value, sizeof( value ) );
    return value;
}


namespace RandomAccessIndex
{
enum class ChecksumType : uint8_t
{
    NONE     = 0,
    CRC_1    = 1,
    CRC_16   = 2,
    CRC_32   = 3,
    CRC_32C  = 4,
    CRC_64   = 5,
    ADLER_32 = 6,
};


[[nodiscard]] const char*
toString( const ChecksumType checksumType )
{
    switch ( checksumType )
    {
    case ChecksumType::NONE    : return "None";
    case ChecksumType::CRC_1   : return "CRC-1";
    case ChecksumType::CRC_16  : return "CRC-16";
    case ChecksumType::CRC_32  : return "CRC-32";
    case ChecksumType::CRC_32C : return "CRC-32C";
    case ChecksumType::CRC_64  : return "CRC-64";
    case ChecksumType::ADLER_32: return "Adler-32";
    }
    return "Unknown";
}


[[nodiscard]] std::optional<size_t>
getChecksumSize( const ChecksumType checksumType )
{
    switch ( checksumType )
    {
    case ChecksumType::NONE    : return 0U;
    case ChecksumType::CRC_1   : return 1U;
    case ChecksumType::CRC_16  : return 2U;
    case ChecksumType::CRC_32  : return 4U;
    case ChecksumType::CRC_32C : return 4U;
    case ChecksumType::CRC_64  : return 8U;
    case ChecksumType::ADLER_32: return 4U;
    }
    return std::nullopt;
}


static constexpr uint8_t SPARSE_FLAG = static_cast<uint8_t>( 1U << 7U );
static constexpr uint8_t WINDOW_COMPRESSION_TYPE_MASK = 0b0111'1111U;

/**
 * Writes the index information out in Random Access Index (RAI) format as defined below:
 *
 * @verbatim
 * Index Format Outline:
 *
 * Offset | Size | Value          | Description
 * -------+------+----------------+---------------------------------------------------
 *      0 |    4 | Magic Bytes:   |  Don't have to be super short, it's negligible compared to a window anyway.
 *        |      | "RAI\x1D"      | Acronyms for Random Access Index and 0x1D representing acronyms for
 *        |      |                | "Index for Decompression" again in order to have some non-printable bytes to avoid
 *        |      |                | misidentifying text files that happen to begin with RAI as this format.
 * -------+------+----------------+---------------------------------------------------
 *      4 |    1 | Format version | Can be thought of to belong to the magic bytes because any two
 *        |      | 0x01           | versions are not ensured to be compatible with each other in any way.
 * -------+------+----------------+---------------------------------------------------
 *      5 |    8 | Archive Size   | Size in bytes of the archive belonging to this index as a 64-bit number.
 * -------+------+----------------+---------------------------------------------------
 *     13 |    1 | Member Flags   | Enable (1) or disable (0) members for the window information:
 *        |      |                | Bit 0: Encoded Size
 *        |      |                | Bit 1: Compressed Window Size
 *        |      |                | Bit 2: Decompressed Window Size
 *        |      |                | Bit 3: Window Offset
 *        |      |                | Bit 4: 0
 *        |      |                | Bit 5: 0
 *        |      |                | Bit 6: 0
 *        |      |                | Bit 7: 0
 *        |      |                | If "Window Offset" is 1, "Compressed Window Size" must also be 1.
 *        |      |                | If "Decompressed Window Size" is 1, "Compressed Window Size" must also be 1.
 * -------+------+----------------+---------------------------------------------------
 *     14 |    1 | Checksum Type  |  0 : None
 *        |      |                |  1 : CRC-1 (parity bit)
 *        |      |                |  2 : CRC-16
 *        |      |                |  3 : CRC-32
 *        |      |                |  4 : CRC-32C
 *        |      |                |  5 : CRC-64
 *        |      |                |  6 : Adler-32
 *        |      |                | https://en.wikipedia.org/wiki/List_of_hash_functions
 * -------+------+----------------+---------------------------------------------------
 *     15 |    1 | Checksum Size  | Similar to Member Flags. A value of 0 disables the checksum member.
 *        |      |                | Positive values represent the number of bytes for the checksum member.
 *        |      |                | This means that checksums are limited to 255 B = 2040 bits.
 *        |      |                | The largest common checksums known to me are 512 bits.
 *        |      |                | https://en.wikipedia.org/wiki/List_of_hash_functions
 *        |      |                | The checkusm size must match the one implied by the checksum type.
 * -------+------+----------------+---------------------------------------------------
 *     16 |    1 | Archive Com-   | The archive compression type. It is redundant to the magic bytes header of
 *        |      | pression Type  | the archive and therefore only functions as a hint or checksum that this index
 *        |      |                | belongs to the correct archive. See "Window Compression Type" for possible values.
 *        |      |                | If the value is 0, it should be ignored by the reader.
 * -------+------+----------------+---------------------------------------------------
 *     17 |    1 | Window Com-    | Value of the lowest 6 bits (bits 0-5):
 *        |      | pression Type  |  0 : None
 *        |      |                |  1 : Deflate
 *        |      |                |  2 : Zlib
 *        |      |                |  3 : Gzip
 *        |      |                |  4 : Bzip2
 *        |      |                |  5 : LZ4
 *        |      |                |  6 : ZStandard
 *        |      |                |  7 : LZMA
 *        |      |                |  8 : XZ
 *        |      |                |  9 : Brotli
 *        |      |                | 10 : lzip
 *        |      |                | 11 : lzop
 *        |      |                | For compatibility, it is recommended to use a similar format to the archive
 *        |      |                | compression type. I.e., only use zlib/gzip/deflate for window compression for
 *        |      |                | zlib/gzip/deflate-compressed archives and only use bzip2 for bzip2-compressed
 *        |      |                | archives.
 *        |      |                | https://en.wikipedia.org/wiki/List_of_archive_formats
 *        |      |                |
 *        |      |                | Bit 6: 0
 *        |      |                | Bit 7: Sparse window format. The (possibly decompressed) window data is in
 *        |      |                |        sparse format as described below.
 * -------+------+----------------+---------------------------------------------------
 *     18 |    8 | Number of      | The number of chunks in the following "list of chunk information".
 *        |      | Chunks         |
 * -------+------+----------------+---------------------------------------------------
 *     26 |    ? | List of Chunk  | The chunk information format is shown below.
 *        |      | Information    |
 * -------+------+----------------+---------------------------------------------------
 *      ? |    ? | List of        | Each window is simply a raw stream of data, which might be compressed.
 *        |      | Windows        | The decompressed window data might also be in sparse format
 *
 *
 * Chunk Information Member
 *  Size | Name           | Description
 * ------+----------------+---------------------------------------------------
 *     8 | encoded offset | Encoded / compressed chunk offset in bits as 64-bit number stored in little endian.
 * ------+----------------+---------------------------------------------------
 *     8 | decoded offset | Decoded / decompressed chunk offset in bytes. The decoded size is always given
 *       |                | by the difference to the next chunk's decoded offset.
 *       |                | This also implies that the last chunk information is not actually used
 *       |                | but must be appended to define the implied "decoded size" and possibly
 *       |                | "encoded size".
 * ------+----------------+---------------------------------------------------
 *     8 | encoded size   | (optional) Encoded / compressed chunk size in bits. If this is not given.
 *       |                | it is to be determined by the difference to the last member offset.
 * ------+----------------+---------------------------------------------------
 *     8 | (compressed)   | (optional) The compressed window size in bits (!). The actually window storage
 *       | window size    | size is rounded up to the next byte because windows must be byte-aligned.
 *       |                | Gzip windows are 32 KiB, LZ4 windows 64 KiB at most.
 *       |                | LZMA supports up to 4 GiB and zstd even larger but storing these
 *       |                | would be unfeasible especially if they are this large after pruning unused symbols
 *       |                | and after compression.
 * ------+----------------+---------------------------------------------------
 *     8 | window offset  | (optional) This offset in bytes is relative to the window data array.
 *       |                | If window offset is not given, it begins at 0 and increases by ceil( "compressed
 *       |                | window size" / 8 ) for the following chunk information. If the compressed window
 *       |                | size is also not given, the windows are assumed to be empty / not necessary.
 *       |                | It probably is not necessary to enabled this member. It could be used to reuse
 *       |                | the same window for multiple chunks, but it will be a rare occurence for this to
 *       |                | make sense.
 * ------+----------------+---------------------------------------------------
 *     8 | decompressed   | (optional) The decompressed window size can be added to aid performance for
 *       | window size    | window decompression or as a kind of check sum. In general, this information is
 *       |                | redundant because it is implicitly given by "(compressed) window size" and the
 *       |                | compressed window data stream. In contrast to the compressed window size, this is in bytes.
 * ------+----------------+---------------------------------------------------
 *     ? | checksum       | (optional) Checksum for the decompressed chunk data.
 *
 * Notes regarding the chunk information:
 *  - The last chunk information is not actually used but must be appended to define the implied "decoded size".
 *    All other entries beside "decoded offset" and "encoded offset" should be set to 0.
 *  - If the "decoded size" is not enabled, chunks must be sorted by decoded offset.
 *  - If the "encoded size" is not enabled, chunks must be sorted by encoded offset.
 *
 * Sparse Window Format:
 *
 *  - Input: vector of known size containing bytes.
 *  - Interpretation of those bytes:
 *    - List of interleaved raw window data vectors and "jumps" over unnecessary data
 *     - Variable-length-encoded size of raw window data (may be 0)
 *     - Raw window data
 *     - Variable-length-encoded size of unknown/unneeded symbols to skip over (may be 0)
 *  - Variable-length encoding:
 *    - 0-127: raw number
 *    - bit 7 set: bits 0-7 are the lowest bits of the number. The rest of the bits follow
 *    -> It basically is a chained list of 7-bit packages that should be shifted from the left to the number
 *       until a byte is encounted which has its highest bit set to 0.
 *    - Example: [1|1000111] [1|0011010] [0|1000001] -> resulting number: 0b1000001'0011010'1000111 = 1068359
 *
 * There are multipe valid use cases for this index:
 *  - bzip2 index file: all optional members except possibly the checksum are disabled because the encoded/decoded
 *    offset are sufficient for random access.
 *  - gzip with uncompressed windows (not recommended)
 *  - zip: multiple compressed streams, which requires the "encoded size" member to effectively skip the zip glue
 *         data.
 *
 * General Notes:
 *
 *  - All multi-byte numbers in the format described here are stored with
 *    the least-significant byte first (at the lower memory address).
 *  - Encoded/compressed offsets and sizes are always in bits, decompressed in bytes
 * @endverbatim
 */
inline void
writeGzipIndex( const GzipIndex&                                              index,
                const std::function<void( const void* buffer, size_t size )>& checkedWrite )
{
    if ( !index.windows ) {
        throw std::invalid_argument( "GzipIndex::windows must be a valid pointer!" );
    }

    const auto writeValue = [&checkedWrite] ( auto value ) { checkedWrite( &value, sizeof( value ) ); };

    const auto& checkpoints = index.checkpoints;

    if ( !std::all_of( checkpoints.begin(), checkpoints.end(), [&index] ( const auto& checkpoint ) {
                           return static_cast<bool>( index.windows->get( checkpoint.compressedOffsetInBits ) );
                       } ) )
    {
        throw std::invalid_argument( "Windows must exist for all offsets!" );
    }

    checkedWrite( /* magic bytes */ "RAI\x1D", 4 );
    checkedWrite( /* format version */ "\x01", 1 );
    writeValue( static_cast<uint64_t>( index.compressedSizeInBytes ) );  // 8 B

    /* Checkpoints do not yet have compressedSizeInBits but it will be added for zip support. */
#if 0
    const auto encodedOffsetsAreConsecutive =
        [&] () {
            for ( size_t i = 0; i + 1 < checkpoints.size(); ++i ) {
                if ( checkpoints[i].compressedOffsetInBits + checkpoints[i].compressedSizeInBits !=
                     checkpoints[i + 1].compressedOffsetInBits )
                {
                    return false;
                }
            }
            return true;
        } ();
    const auto hasEncodedSize = !encodedOffsetsAreConsecutive;
#else
    const auto hasEncodedSize = false;
#endif

    const auto hasNonEmptyWindows =
            std::any_of( checkpoints.begin(), checkpoints.end(), [&index] ( const auto& checkpoint ) {
                const auto window = index.windows->get( checkpoint.compressedOffsetInBits );
                return window && !window->empty();
            } );
    const auto hasCompressedWindowSize = hasNonEmptyWindows;

    const auto hasDecompressedWindowSize = true;
    const auto hasWindowOffset = false;  // Windows are stored consecutively and therefore need no offset.

    const uint8_t flags = static_cast<uint8_t>( ( ( hasWindowOffset ? 1U : 0U ) << 3U )
                                                | ( ( hasDecompressedWindowSize ? 1U : 0U ) << 2U )
                                                | ( ( hasCompressedWindowSize ? 1U : 0U ) << 1U )
                                                | ( hasEncodedSize ? 1U : 0U ) );
    writeValue( flags );  // 1 B

    /** @todo compute CRC32 checksums for each chunk and forward them to GzipIndex and write them out. */
    const auto checksumType = ChecksumType::NONE;
    writeValue( static_cast<uint8_t>( checksumType ) );  // 1 B
    const uint8_t checksumSize{ 0 };
    writeValue( checksumSize );  // 1 B

    /** @todo Correctly support indexes for zlib and deflate archives. */
    const auto archiveCompressionType = CompressionType::GZIP;
    writeValue( static_cast<uint8_t>( archiveCompressionType ) );  // 1 B

    /* Using gzip instead of deflate adds a bit of overhead for the header and footer but for most windows,
     * this should be negligible and it has the advantage that each window is checksummed thanks to the gzip footer. */
    const auto windowCompressionType = index.windows->compressionType();
    auto windowCompression = static_cast<uint8_t>( windowCompressionType );
    const bool sparseCompression = false;  /** @todo implement this inside CompressedVector. */
    if ( sparseCompression ) {
        windowCompression |= SPARSE_FLAG;
    }
    writeValue( windowCompression );  // 1 B

    writeValue( static_cast<uint64_t>( checkpoints.size() ) );  // 8 B

    /* Write out list of chunk informations. */

    const auto& [lock, windows] = index.windows->data();

    for ( const auto& checkpoint : checkpoints ) {
        writeValue( static_cast<uint64_t>( checkpoint.compressedOffsetInBits ) );
        writeValue( static_cast<uint64_t>( checkpoint.uncompressedOffsetInBytes ) );
    #if 0
        if ( hasEncodedSize ) {
            writeValue( static_cast<uint64_t>( checkpoint.compressedSizeInBits ) );
        }
    #endif

        if ( hasCompressedWindowSize ) {
            const auto window = *windows->at( checkpoint.compressedOffsetInBits );
            writeValue( static_cast<uint64_t>( window.compressedSize() * 8U ) );

            if ( hasWindowOffset ) {
                throw std::logic_error( "Window offset not supported yet because it only adds overhead!" );
            }

            if ( hasDecompressedWindowSize ) {
                writeValue( static_cast<uint64_t>( window.decompressedSize() ) );
            }
        } else {
            if ( hasWindowOffset ) {
                throw std::logic_error( "Window offset has no meaning without compressed window size!" );
            }
            if ( hasDecompressedWindowSize ) {
                throw std::logic_error( "Decompressed window size has no meaning without compressed window size!" );
            }
        }

        if ( checksumSize > 0 ) {
            throw std::logic_error( "Checksum writing not yet implemented!" );
        }
    }

    /* Write out compressed window data. */

    for ( const auto& checkpoint : checkpoints ) {
        const auto window = *windows->at( checkpoint.compressedOffsetInBits );
        if ( ( window.compressedSize() == 0 ) && ( window.compressionType() != windowCompressionType ) ) {
            std::stringstream message;
            message << "All windows inside the WindowMap should have the same compression type because recompression "
                    << "has not been implemented yet. Expected: " << toString( windowCompressionType ) << " but got "
                    << " window compression: " << toString( window.compressionType() );
            throw std::logic_error( std::move( message ).str() );
        }
        const auto& compressedData = window.compressedData();
        checkedWrite( compressedData.data(), compressedData.size() );
    }
}


[[nodiscard]] inline GzipIndex
readGzipIndex( UniqueFileReader            indexFile,
               const std::optional<size_t> archiveSize = std::nullopt,
               const std::vector<char>&    alreadyReadBytes = {} )
{
    if ( !indexFile ) {
        throw std::invalid_argument( "Index file reader must be valid!" );
    }
    static constexpr size_t MAGIC_BYTE_COUNT = 5U;
    if ( alreadyReadBytes.size() > MAGIC_BYTE_COUNT ) {
        throw std::invalid_argument( "This function only supports skipping up to over the magic bytes if given." );
    }
    if ( alreadyReadBytes.size() != indexFile->tell() ) {
        throw std::invalid_argument( "The file position must match the number of given bytes." );
    }

    auto magicBytes = alreadyReadBytes;
    if ( magicBytes.size() < MAGIC_BYTE_COUNT ) {
        const auto oldSize = magicBytes.size();
        magicBytes.resize( MAGIC_BYTE_COUNT );
        checkedRead( indexFile.get(), magicBytes.data() + oldSize, magicBytes.size() - oldSize );
    }

    const std::string_view MAGIC_BYTES = "RAI\x1D";
    if ( !std::equal( MAGIC_BYTES.begin(), MAGIC_BYTES.end(), magicBytes.begin() ) ) {
        throw std::invalid_argument( "Magic bytes do not match! Expected 'GZIDX'." );
    }

    const auto formatVersion = static_cast<uint8_t>( magicBytes[magicBytes.size() - 1] );
    if ( formatVersion > 1 ) {
        throw std::invalid_argument( "Index was written with a newer rapidgzip version than supported!" );
    }

    /* Read index header. */
    GzipIndex index;
    index.compressedSizeInBytes = readValue<uint64_t>( indexFile.get() );
    const auto memberFlags = readValue<uint8_t>( indexFile.get() );
    const auto checksumType = static_cast<ChecksumType>( readValue<uint8_t>( indexFile.get() ) );
    const auto checksumSize = readValue<uint8_t>( indexFile.get() );
    const auto archiveCompressionType = static_cast<CompressionType>( readValue<uint8_t>( indexFile.get() ) );
    const auto windowCompression = readValue<uint8_t>( indexFile.get() );
    const auto chunkCount = readValue<uint64_t>( indexFile.get() );

    /* Check archive size and type. */
    if ( archiveSize && ( *archiveSize != index.compressedSizeInBytes ) ) {
        std::stringstream message;
        message << "Archive size does not match! Archive is " << formatBytes( *archiveSize )
                << " but index has stored an archive size of " << formatBytes( index.compressedSizeInBytes )
                << "!";
        throw std::invalid_argument( std::move( message ).str() );
    }

    if ( archiveCompressionType != CompressionType::GZIP ) {
        /** @todo Add support for zlib and deflate. */
        throw std::invalid_argument( "Currently, only gzip archives are supported!" );
    }

    /* Check flags for validity. */
    const auto hasEncodedSize            = ( memberFlags & 1U ) != 0U;
    const auto hasCompressedWindowSize   = ( memberFlags & ( 1U << 1U ) ) != 0U;
    const auto hasDecompressedWindowSize = ( memberFlags & ( 1U << 2U ) ) != 0U;
    const auto hasWindowOffset           = ( memberFlags & ( 1U << 3U ) ) != 0U;

    if ( hasWindowOffset && !hasCompressedWindowSize ) {
        throw std::invalid_argument( "Window offset member makes no sense without the compressed window size!" );
    }
    if ( hasDecompressedWindowSize && !hasCompressedWindowSize ) {
        throw std::invalid_argument( "Decompressed window size makes no sense without the compressed window size!" );
    }
    if ( ( memberFlags >> 4U ) != 0U ) {
        throw std::invalid_argument( "The higher member flag bits are set even though they should be unused at 0!" );
    }

    /* Check compression types for validity. */
    const auto sparseFlag = ( windowCompression & SPARSE_FLAG ) != 0U;
    if ( sparseFlag ) {
        /** @todo Add support for this inside the CompressedVector interface! */
        throw std::invalid_argument( "Sparse window compression not yet supported!" );
    }
    const auto windowCompressionType = static_cast<CompressionType>( windowCompression & WINDOW_COMPRESSION_TYPE_MASK );

    const std::unordered_set supportedCompressionTypes = { CompressionType::NONE, CompressionType::GZIP };
    if ( !contains( supportedCompressionTypes, windowCompressionType ) ) {
        std::stringstream message;
        message << "Window compression type " << toString( windowCompressionType ) << " is currently not supported!";
        throw std::invalid_argument( std::move( message ).str() );
    }
    const auto expectedChecksumSize = getChecksumSize( checksumType );
    if ( expectedChecksumSize && ( checksumSize != expectedChecksumSize ) ) {
        std::stringstream message;
        message << "Expected checksum size for " << toString( checksumType ) << " to be "
                << formatBytes( *expectedChecksumSize ) << " but got: " << formatBytes( checksumSize ) << "!";
        throw std::invalid_argument( std::move( message ).str() );
    }

    std::vector<std::pair<size_t, size_t> > windowSizes;
    std::vector<size_t> checksum( checksumSize );

    /* Read chunk info metadata. */
    for ( size_t i = 0; i < chunkCount; ++i ) {
        auto& checkpoint = index.checkpoints.emplace_back();
        checkpoint.compressedOffsetInBits = readValue<uint64_t>( indexFile.get() );
        checkpoint.uncompressedOffsetInBytes = readValue<uint64_t>( indexFile.get() );

        if ( hasEncodedSize ) {
            /** @todo Support for this should be added with ZIP support. */
            throw std::invalid_argument( "Indexes with independent encoded chunk sizes are not supported yet!" );
        }

        size_t compressedWindowSize{ 0 };
        size_t decompressedWindowSize{ 0 };

        if ( hasCompressedWindowSize ) {
            compressedWindowSize = readValue<uint64_t>( indexFile.get() );
            if ( !hasDecompressedWindowSize ) {
                throw std::invalid_argument( "The decompressed window size is currently required if there are "
                                             "windows!" );
            }
        }
        if ( hasWindowOffset ) {
            throw std::invalid_argument( "Indexes with independent window offset not supported yet!" );
        }
        if ( hasDecompressedWindowSize ) {
            /* Ignore for now. Could be used to allocate the decompression buffer or check the decompressed windows. */
            decompressedWindowSize = readValue<uint64_t>( indexFile.get() );
        }
        /** @todo fully add checksum data to the checkpoint and verify it during decompression. */
        checkedRead( indexFile.get(), checksum.data(), checksum.size() );

        if ( hasCompressedWindowSize ) {
            windowSizes.emplace_back( compressedWindowSize, decompressedWindowSize );
        }
    }

    /* Read window data. */
    index.windows = std::make_shared<WindowMap>();
    for ( size_t i = 0; i < index.checkpoints.size(); ++i ) {
        const auto& checkpoint = index.checkpoints.at( i );

        if ( !hasCompressedWindowSize ) {
            index.windows->emplaceShared( checkpoint.compressedOffsetInBits, {} );
            continue;
        }

        const auto [windowSize, decompressedWindowSize] = windowSizes[i];
        if ( windowSize % 8U != 0U ) {
            if ( windowCompressionType != CompressionType::DEFLATE ) {
                std::stringstream message;
                message << "Non-byte-aligned window sizes only make sense for deflate compression but the compression "
                        << "type is: " << toString( windowCompressionType ) << "!";
                throw std::logic_error( std::move( message ).str() );
            }
            throw std::invalid_argument( "Non-byte-aligned window sizes are not supported yet!" );
        }

        FasterVector<uint8_t> windowData( ceilDiv( windowSize, 8U ) );
        checkedRead( indexFile.get(), windowData.data(), windowData.size() );
        /** @todo add support to automatically determine the decompressed size. */
        index.windows->emplaceShared( checkpoint.compressedOffsetInBits,
                                      std::make_shared<WindowMap::Window>( std::move( windowData ),
                                                                           decompressedWindowSize,
                                                                           windowCompressionType ) );
    }

    return index;
}
}  // RandomAccessIndex


namespace bgzip
{
[[nodiscard]] size_t
countDecompressedBytes( rapidgzip::BitReader           bitReader,
                        VectorView<std::uint8_t> const initialWindow )
{
    #ifdef WITH_ISAL
        using InflateWrapper = rapidgzip::IsalInflateWrapper;
    #else
        using InflateWrapper = rapidgzip::ZlibInflateWrapper;
    #endif

    InflateWrapper inflateWrapper( std::move( bitReader ), std::numeric_limits<size_t>::max() );
    inflateWrapper.setWindow( initialWindow );

    size_t alreadyDecoded{ 0 };
    std::vector<uint8_t> subchunk( 128_Ki );
    while ( true ) {
        std::optional<InflateWrapper::Footer> footer;
        size_t nBytesReadPerCall{ 0 };
        while ( !footer ) {
            std::tie( nBytesReadPerCall, footer ) = inflateWrapper.readStream( subchunk.data(), subchunk.size() );
            if ( nBytesReadPerCall == 0 ) {
                break;
            }
            alreadyDecoded += nBytesReadPerCall;
        }

        if ( ( nBytesReadPerCall == 0 ) && !footer ) {
            break;
        }
    }

    return alreadyDecoded;
}


[[nodiscard]] inline GzipIndex
readGzipIndex( UniqueFileReader         indexFile,
               UniqueFileReader         archiveFile = {},
               const std::vector<char>& alreadyReadBytes = {} )
{
    if ( !indexFile ) {
        throw std::invalid_argument( "Index file reader must be valid!" );
    }
    if ( alreadyReadBytes.size() != indexFile->tell() ) {
        throw std::invalid_argument( "The file position must match the number of given bytes." );
    }
    static constexpr size_t MAGIC_BYTE_COUNT = sizeof( uint64_t );
    if ( alreadyReadBytes.size() > MAGIC_BYTE_COUNT ) {
        throw std::invalid_argument( "This function only supports skipping up to over the magic bytes if given." );
    }

    /* We need a seekable archive to add the very first and very last offset pairs.
     * If the archive is not seekable, loading the index makes not much sense anyways.
     * If it is still needed, then use a better index file format instead of BGZI. */
    if ( !archiveFile || !archiveFile->size().has_value() ) {
        throw std::invalid_argument( "Cannot import bgzip index without knowing the archive size!" );
    }
    const auto archiveSize = archiveFile->size();

    /**
     * Try to interpret it as BGZF index, which is simply a list of 64-bit values stored in little endian:
     * uint64_t number_entries
     * [Repated number_entries times]:
     *     uint64_t compressed_offset
     *     uint64_t uncompressed_offset
     * Such an index can be created with: bgzip -c file > file.bgz; bgzip --reindex file.bgz
     * @see http://www.htslib.org/doc/bgzip.html#GZI_FORMAT
     * @note by reusing the already read 5 bytes we can avoid any seek, making it possible to work
     *       with a non-seekable input although I doubt it will be used.
     */
    uint64_t numberOfEntries{ 0 };
    std::memcpy( &numberOfEntries, alreadyReadBytes.data(), alreadyReadBytes.size() );
    checkedRead( indexFile.get(),
                 reinterpret_cast<char*>( &numberOfEntries ) + alreadyReadBytes.size(),
                 sizeof( uint64_t ) - alreadyReadBytes.size() );

    GzipIndex index;

    /* I don't understand why bgzip writes out 0xFFFF'FFFF'FFFF'FFFFULL in case of an empty file
     * instead of simply 0, but it does. */
    if ( numberOfEntries == std::numeric_limits<uint64_t>::max() ) {
        numberOfEntries = 0;  // Set it to a sane value which also will make the file size check work.
        index.compressedSizeInBytes = 0;
        index.uncompressedSizeInBytes = 0;
    }

    const auto expectedFileSize = ( 2U * numberOfEntries + 1U ) * sizeof( uint64_t );
    if ( ( indexFile->size() > 0 ) && ( indexFile->size() != expectedFileSize ) ) {
        throw std::invalid_argument( "Invalid magic bytes!" );
    }
    index.compressedSizeInBytes = *archiveSize;

    index.checkpoints.reserve( numberOfEntries + 1 );

    const auto sharedArchiveFile = ensureSharedFileReader( std::move( archiveFile ) );

    try {
        rapidgzip::blockfinder::Bgzf blockfinder( sharedArchiveFile->clone() );
        const auto firstBlockOffset = blockfinder.find();
        if ( firstBlockOffset == std::numeric_limits<size_t>::max() ) {
            throw std::invalid_argument( "" );
        }

        auto& firstCheckPoint = index.checkpoints.emplace_back();
        firstCheckPoint.compressedOffsetInBits = firstBlockOffset;
        firstCheckPoint.uncompressedOffsetInBytes = 0;
    } catch ( const std::invalid_argument& exception ) {
        std::stringstream message;
        message << "Trying to load a BGZF index for a non-BGZF file!";
        const std::string_view what( exception.what() );
        if ( !what.empty() ) {
            message << " (" << what << ")";
        }
        throw std::invalid_argument( std::move( message ).str() );
    }

    index.windows = std::make_shared<WindowMap>();

    for ( uint64_t i = 1; i < numberOfEntries; ++i ) {
        auto& checkpoint = index.checkpoints.emplace_back();
        checkpoint.compressedOffsetInBits = readValue<uint64_t>( indexFile.get() );
        checkpoint.uncompressedOffsetInBytes = readValue<uint64_t>( indexFile.get() );
        checkpoint.compressedOffsetInBits += 18U;  // Jump over gzip header
        checkpoint.compressedOffsetInBits *= 8U;

        const auto& lastCheckPoint = *( index.checkpoints.rbegin() + 1 );

        if ( checkpoint.compressedOffsetInBits > index.compressedSizeInBytes * 8U ) {
            std::stringstream message;
            message << "Compressed bit offset (" << checkpoint.compressedOffsetInBits
                    << ") should be smaller or equal than the file size ("
                    << index.compressedSizeInBytes * 8U << ")!";
            throw std::invalid_argument( std::move( message ).str() );
        }

        if ( checkpoint.compressedOffsetInBits <= lastCheckPoint.compressedOffsetInBits ) {
            std::stringstream message;
            message << "Compressed bit offset (" << checkpoint.compressedOffsetInBits
                    << ") should be greater than predecessor ("
                    << lastCheckPoint.compressedOffsetInBits << ")!";
            throw std::invalid_argument( std::move( message ).str() );
        }

        if ( checkpoint.uncompressedOffsetInBytes < lastCheckPoint.uncompressedOffsetInBytes ) {
            std::stringstream message;
            message << "Uncompressed offset (" << checkpoint.uncompressedOffsetInBytes
                    << ") should be greater or equal than predecessor ("
                    << lastCheckPoint.uncompressedOffsetInBytes << ")!";
            throw std::invalid_argument( std::move( message ).str() );
        }

        /* Emplace an empty window to show that the block does not need data. */
        index.windows->emplace( checkpoint.compressedOffsetInBits, {} );
    }

    try {
        rapidgzip::BitReader bitReader( sharedArchiveFile->clone() );
        bitReader.seek( index.checkpoints.back().compressedOffsetInBits );
        index.uncompressedSizeInBytes = index.checkpoints.back().uncompressedOffsetInBytes
                                        + countDecompressedBytes( std::move( bitReader ), {} );
    } catch ( const std::invalid_argument& ) {
        throw std::invalid_argument( "Unable to read from the last given offset in the index!" );
    }

    return index;
}
}  // namespace bgzip


namespace indexed_gzip
{
[[nodiscard]] inline GzipIndex
readGzipIndex( UniqueFileReader            indexFile,
               const std::optional<size_t> archiveSize = std::nullopt,
               const std::vector<char>&    alreadyReadBytes = {} )
{
    if ( !indexFile ) {
        throw std::invalid_argument( "Index file reader must be valid!" );
    }
    if ( alreadyReadBytes.size() != indexFile->tell() ) {
        throw std::invalid_argument( "The file position must match the number of given bytes." );
    }
    static constexpr size_t MAGIC_BYTE_COUNT = 6U;
    if ( alreadyReadBytes.size() > MAGIC_BYTE_COUNT ) {
        throw std::invalid_argument( "This function only supports skipping up to over the magic bytes if given." );
    }

    auto magicBytes = alreadyReadBytes;
    if ( magicBytes.size() < MAGIC_BYTE_COUNT ) {
        const auto oldSize = magicBytes.size();
        magicBytes.resize( MAGIC_BYTE_COUNT );
        checkedRead( indexFile.get(), magicBytes.data() + oldSize, magicBytes.size() - oldSize );
    }

    const std::string_view MAGIC_BYTES = "GZIDX";
    if ( !std::equal( MAGIC_BYTES.begin(), MAGIC_BYTES.end(), magicBytes.begin() ) ) {
        throw std::invalid_argument( "Magic bytes do not match! Expected 'GZIDX'." );
    }

    const auto formatVersion = static_cast<uint8_t>( magicBytes[magicBytes.size() - 1] );
    if ( formatVersion > 1 ) {
        throw std::invalid_argument( "Index was written with a newer indexed_gzip version than supported!" );
    }

    indexFile->seek( 1, SEEK_CUR );  // Skip reserved flags 1B

    GzipIndex index;
    index.compressedSizeInBytes   = readValue<uint64_t>( indexFile.get() );
    index.uncompressedSizeInBytes = readValue<uint64_t>( indexFile.get() );
    index.checkpointSpacing       = readValue<uint32_t>( indexFile.get() );
    index.windowSizeInBytes       = readValue<uint32_t>( indexFile.get() );

    if ( archiveSize && ( *archiveSize != index.compressedSizeInBytes ) ) {
        std::stringstream message;
        message << "File size for the compressed file (" << *archiveSize
                << ") does not fit the size stored in the given index (" << index.compressedSizeInBytes << ")!";
        throw std::invalid_argument( std::move( message ).str() );
    }

    /* However, a window size larger than 32 KiB makes no sense bacause the Lempel-Ziv back-references
     * in the deflate format are limited to 32 KiB! Smaller values might, however, be enforced by especially
     * memory-constrained encoders.
     * This basically means that we either check for this to be exactly 32 KiB or we simply throw away all
     * other data and only load the last 32 KiB of the window buffer. */
    if ( index.windowSizeInBytes != 32_Ki ) {
        throw std::invalid_argument( "Only a window size of 32 KiB makes sense because indexed_gzip supports "
                                     "no smaller ones and gzip does support any larger one." );
    }
    const auto checkpointCount = readValue<uint32_t>( indexFile.get() );

    std::vector<std::pair</* encoded offset */ size_t, /* window size */ size_t> > windowInfos;

    index.checkpoints.resize( checkpointCount );
    for ( uint32_t i = 0; i < checkpointCount; ++i ) {
        auto& checkpoint = index.checkpoints[i];

        /* First load only compressed offset rounded down in bytes, the bits are loaded down below! */
        checkpoint.compressedOffsetInBits = readValue<uint64_t>( indexFile.get() );
        if ( checkpoint.compressedOffsetInBits > index.compressedSizeInBytes ) {
            throw std::invalid_argument( "Checkpoint compressed offset is after the file end!" );
        }
        checkpoint.compressedOffsetInBits *= 8;

        checkpoint.uncompressedOffsetInBytes = readValue<uint64_t>( indexFile.get() );
        if ( checkpoint.uncompressedOffsetInBytes > index.uncompressedSizeInBytes ) {
            throw std::invalid_argument( "Checkpoint uncompressed offset is after the file end!" );
        }

        const auto bits = readValue<uint8_t>( indexFile.get() );
        if ( bits >= 8 ) {
            throw std::invalid_argument( "Denormal compressed offset for checkpoint. Bit offset >= 8!" );
        }
        if ( bits > 0 ) {
            if ( checkpoint.compressedOffsetInBits == 0 ) {
                throw std::invalid_argument( "Denormal bits for checkpoint. Effectively negative offset!" );
            }
            checkpoint.compressedOffsetInBits -= bits;
        }

        size_t windowSize{ 0 };
        if ( formatVersion == 0 ) {
            if ( i != 0 ) {
                windowSize = index.windowSizeInBytes;
            }
        } else {
            if ( /* data flag */ readValue<uint8_t>( indexFile.get() ) != 0 ) {
                windowSize = index.windowSizeInBytes;
            }
        }
        windowInfos.emplace_back( checkpoint.compressedOffsetInBits, windowSize );
    }

    index.windows = std::make_shared<WindowMap>();
    for ( auto& [offset, windowSize] : windowInfos ) {
        FasterVector<uint8_t> window;
        if ( windowSize > 0 ) {
            window.resize( windowSize );
            checkedRead( indexFile.get(), window.data(), window.size() );
        }
        index.windows->emplace( offset, window );
    }

    return index;
}
}  // namespace indexed_gzip


[[nodiscard]] inline GzipIndex
readGzipIndex( UniqueFileReader indexFile,
               UniqueFileReader archiveFile = {} )
{
    std::vector<char> formatId( 5, 0 );
    checkedRead( indexFile.get(), formatId.data(), formatId.size() );

    std::optional<size_t> archiveSize;
    if ( archiveFile ) {
        archiveSize = archiveFile->size();
    }

    if ( formatId == std::vector<char>( { 'R', 'A', 'I', '\x1D', '\x01' } ) ) {
        return RandomAccessIndex::readGzipIndex( std::move( indexFile ), archiveSize, formatId );
    }
    if ( formatId == std::vector<char>( { 'G', 'Z', 'I', 'D', 'X' } ) ) {
        return indexed_gzip::readGzipIndex( std::move( indexFile ), archiveSize, formatId );
    }
    /* Bgzip indexes have no magic bytes and simply start with the number of chunks. */
    return bgzip::readGzipIndex( std::move( indexFile ), std::move( archiveFile ), formatId );
}


inline void
writeGzipIndex( const GzipIndex&                                              index,
                const std::function<void( const void* buffer, size_t size )>& checkedWrite )
{
    const auto writeValue = [&checkedWrite] ( auto value ) { checkedWrite( &value, sizeof( value ) ); };

    const auto& checkpoints = index.checkpoints;
    const uint32_t windowSizeInBytes = static_cast<uint32_t>( 32_Ki );

    if ( !std::all_of( checkpoints.begin(), checkpoints.end(), [&index, windowSizeInBytes] ( const auto& checkpoint ) {
                           const auto window = index.windows->get( checkpoint.compressedOffsetInBits );
                           return window && ( window->empty() || ( window->decompressedSize() >= windowSizeInBytes ) );
                       } ) )
    {
        throw std::invalid_argument( "All window sizes must be at least 32 KiB or empty!" );
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
        if ( !index.windows->get( checkpoint.compressedOffsetInBits ) ) {
            throw std::logic_error( "Did not find window to offset " +
                                    formatBits( checkpoint.compressedOffsetInBits ) );
        }
        writeValue( static_cast<uint8_t>( index.windows->get( checkpoint.compressedOffsetInBits )->empty() ? 0 : 1 ) );
    }

    for ( const auto& checkpoint : checkpoints ) {
        const auto result = index.windows->get( checkpoint.compressedOffsetInBits );
        if ( !result ) {
            continue;
        }

        const auto window = result->decompress();
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
