#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include <core/common.hpp>                   // _Ki literals
#include <core/FileUtils.hpp>
#include <core/ThreadPool.hpp>
#include <core/VectorView.hpp>
#include <filereader/FileReader.hpp>
#include <rapidgzip/blockfinder/Bgzf.hpp>
#ifdef LIBRAPIDARCHIVE_WITH_ISAL
    #include <rapidgzip/gzip/isal.hpp>
#endif
#include <rapidgzip/gzip/zlib.hpp>

#include "WindowMap.hpp"


namespace rapidgzip
{
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
    uint64_t lineOffset{ 0 };

    [[nodiscard]] constexpr bool
    operator==( const Checkpoint& other ) const noexcept
    {
        return ( compressedOffsetInBits == other.compressedOffsetInBits ) &&
               ( uncompressedOffsetInBytes == other.uncompressedOffsetInBytes ) &&
               ( lineOffset == other.lineOffset );
    }
};


enum class IndexFormat
{
    INDEXED_GZIP = 0,
    GZTOOL = 1,
    GZTOOL_WITH_LINES = 2,
};


enum class NewlineFormat
{
    LINE_FEED = 0,
    CARRIAGE_RETURN = 1,
};


inline std::ostream&
operator<<( std::ostream& out,
            NewlineFormat newlineFormat )
{
    switch ( newlineFormat )
    {
    case NewlineFormat::LINE_FEED:
        out << "\\n";
        break;
    case NewlineFormat::CARRIAGE_RETURN:
        out << "\\r";
        break;
    }
    return out;
}


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
    /** Must be sorted by Checkpoint::compressedOffsetInBits and Checkpoint::uncompressedOffsetInBytes. */
    std::vector<Checkpoint> checkpoints;

    std::shared_ptr<WindowMap> windows;

    bool hasLineOffsets{ false };
    NewlineFormat newlineFormat{ NewlineFormat::LINE_FEED };

    [[nodiscard]] constexpr bool
    operator==( const GzipIndex& other ) const noexcept
    {
        // *INDENT-OFF*
        return ( compressedSizeInBytes   == other.compressedSizeInBytes   ) &&
               ( uncompressedSizeInBytes == other.uncompressedSizeInBytes ) &&
               ( checkpointSpacing       == other.checkpointSpacing       ) &&
               ( windowSizeInBytes       == other.windowSizeInBytes       ) &&
               ( checkpoints             == other.checkpoints             ) &&
               ( hasLineOffsets          == other.hasLineOffsets          ) &&
               ( newlineFormat           == other.newlineFormat           ) &&
               ( ( windows == other.windows ) || ( windows && other.windows && ( *windows == *other.windows ) ) );
        // *INDENT-ON*
    }
};


inline std::ostream&
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


inline void
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
    /* Note that indexed_gzip itself does no endianness check or conversion during writing,
     * so this system-specific reading is as portable as it gets assuming that the indexes are
     * read on the same system they are written. */
    T value;
    checkedRead( file, &value, sizeof( value ) );
    return value;
}


template<typename T>
[[nodiscard]] T
readBigEndianValue( FileReader* const file )
{
    T value;
    checkedRead( file, &value, sizeof( value ) );
    if ( ENDIAN == Endian::LITTLE ) {
        auto* const buffer = reinterpret_cast<char*>( &value );
        for ( size_t i = 0; i < sizeof( T ) / 2; ++i ) {
            std::swap( buffer[i], buffer[sizeof( T ) - 1 - i] );
        }
    }
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


[[nodiscard]] constexpr const char*
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


[[nodiscard]] constexpr std::optional<size_t>
getChecksumSize( const ChecksumType checksumType )
{
    switch ( checksumType )
    {
    case ChecksumType::NONE:
        return 0U;
    case ChecksumType::CRC_1:
        return 1U;
    case ChecksumType::CRC_16:
        return 2U;
    case ChecksumType::CRC_32:
    case ChecksumType::CRC_32C:
    case ChecksumType::ADLER_32:
        return 4U;
    case ChecksumType::CRC_64:
        return 8U;
    }
    return std::nullopt;
}


static constexpr uint8_t SPARSE_FLAG = static_cast<uint8_t>( 1U << 7U );
static constexpr uint8_t WINDOW_COMPRESSION_TYPE_MASK = 0b0111'1111U;


/**
 * Considerations:
 *
 *  - https://stackoverflow.com/questions/3321592/how-do-you-create-a-file-format
 *  - https://softwareengineering.stackexchange.com/questions/171201/considerations-when-designing-a-file-type
 *  - https://fadden.com/tech/file-formats.htm
 *  - https://web.archive.org/web/20250118102842/https://games.greggman.com/game/zip-rant/
 *  - https://solhsa.com/oldernews2025.html#ON-FILE-FORMATS
 *  - https://news.ycombinator.com/item?id=44049252
 *
 * Motivation:
 *  - 2025-08-08: I have forgotten why I needed this. I think I did not yet know of gztool at that point.
 *                https://github.com/circulosmeos/gztool#index-file-format
 *  - Pros over gztool index format:
 *    - I would add per-chunk checksums.
 *    - Would store sparse information, which could be used for additional correctness checking,
 *      but probably redundant to per chunk checksums.
 *    - The window compression in gztool seems to be undefined, but it uses deflate.
 *      Using zlib would make a checksum redundant, although the checksum is not very strong.
 *      Using other compressions would be important for LZ4 and BZ2 support.
 *      At this point, my gztool index format would already kinda fork the original one,
 *      but because the compressed window is prefixed with the length, it should be mostly compatible,
 *      and by forcing the compression type to match the source archive format (zlib/deflate/gzip for gzip files,
 *      lz4 window compression for index lz4 files, ...), it would stay compatible for gztool's domain of gzip files.
 *    - Storing the window offset would make seeking in the index faster. Gztool index has the problem that
 *      extracting a certain offset range or line needs to load the whole index. By having chunk information separate
 *      and first, we only need to load that and can only load the window data needed directly from the window offset.
 *      -> However, this limits index streaming, which seems to be a use case for gztool for adding to / completing
 *         indexes of growing files through repeated calls, e.g., compressed logs.
 *
 * Writes the index information out in Random Access Index (RAI) format as defined below:
 *
 * @verbatim
 * Index Format Outline:
 *
 * !!!!! I would refactor this to be extensible by using a generic key-value scheme, especially for the
 *       "root" data, whose storage size is unimportant. Chunk information might be stored flat to save size,
 *       but the windows should be the worst offender for storage size anyway, so I don't think even this is necessary!
 * !!!!! Store every "member" (key-value) as a list of skippable formats:
 *       File := List[<size> <member>]
 *       size := "varint", any value other than 0 is the length. 0 denotes that the next 8 bits are a 64-bit number
 *       member := <size> <char string for the information key / ID> <size> <value>
 * !!! That's it! Having each information skippable because of the prefixed varint makes it backward compatible
 *     out of the box. The format is also very easy to implement and parse. Keys can be added and removed.
 *     Any required keys should be documented and enforced via manual checks instead.
 * !!! This also makes it easy to have optional keys such as checksums and so on.
 * !!! It is not fully self-describing. Knowledge about values have to be shared via the specification and matched
 *     by the unique char string ID.
 * I would almost have liked to use something like JSON, but with binary data, so something like BSON.
 * But most serialization libraries do not offer interfaces for direct file access to large members, e.g., via mmap,
 * i.e., everything has to fit into memory. And even streaming is a problem for most of these formats!
 * -> See my old ratarmount serialization backend benchmarks before I was using SQLite!
 * https://github.com/mxmlnkn/ratarmount/blob/master/benchmarks/BENCHMARKS.md
 *      #benchmarks-for-the-index-file-serialization-backends
 * -> Something like protobuf or flatbuf sounded like good contenders. But protobuf seems too opaque and static
 *    and therefore with questionable compatibility. Flatbuf is one of the few that offers direct views to large
 *    members, but well, I forgot what was wrong with it.
 * -> MsgPack has the streaming problem, at least not easily.
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
 *        |      |                | The checksum size must match the one implied by the checksum type.
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
 *       |                | the same window for multiple chunks, but it will be a rare occurrence for this to
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
 *       until a byte is encountered which has its highest bit set to 0.
 *    - Example: [1|1000111] [1|0011010] [0|1000001] -> resulting number: 0b1000001'0011010'1000111 = 1068359
 *
 * There are multiple valid use cases for this index:
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
static constexpr std::string_view MAGIC_BYTES = "RAI\x1D";


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

    const auto flags = static_cast<uint8_t>( ( ( hasWindowOffset ? 1U : 0U ) << 3U )
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
    const auto windowCompressionType = CompressionType::GZIP;
    auto windowCompression = static_cast<uint8_t>( windowCompressionType );
    const bool sparseCompression = false;  /** @todo implement this inside CompressedVector. */
    if ( sparseCompression ) {
        windowCompression |= SPARSE_FLAG;
    }
    writeValue( windowCompression );  // 1 B

    writeValue( static_cast<uint64_t>( checkpoints.size() ) );  // 8 B

    /* Write out list of chunk information. */

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
            /** @todo compress windows in parallel if not already compressed!
             *        There is a problem here! EIther I have to the compression twice only to find out the compressed
             *        size to write out, or we have to hold all recompressed windows in memory!
             *  @todo Maybe it really would be better to require all window compression types to be the same
             *        and recompress them outside in parallel before calling writeGzipIndex.
             *  @todo Or we might want to remember the file positions and then update the compressed sizes
             *        as a post-processing step... But then we couldn't stream anymore.
             *  @todo Or we could change the index format and order the windows contents before the metadata :/
             *        like zip.
             *  @todo Or we could allow mixed compression types. At least compression type non seems useful
             *        to avoid unnecessary copies for decompression
             */
            if ( ( window.compressionType() == windowCompressionType ) || window.empty() ) {
                writeValue( static_cast<uint64_t>( window.empty() ? 0U : window.compressedSize() * 8U ) );
            } else {
                const auto& decompressed = window.decompress();
                if ( !decompressed ) {
                    throw std::logic_error( "Did not get decompressed data for window!" );
                }

                const WindowMap::Window recompressed( *decompressed, windowCompressionType );
                writeValue( static_cast<uint64_t>( recompressed.compressedSize() * 8U ) );
            }

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
        if ( window.empty() ) {
            continue;
        }

        if ( window.compressionType() == windowCompressionType ) {
            const auto& compressedData = window.compressedData();
            if ( !compressedData ) {
                throw std::logic_error( "Did not get compressed data for window!" );
            }
            checkedWrite( compressedData->data(), compressedData->size() );
        } else {
            const auto& decompressed = window.decompress();
            if ( !decompressed ) {
                throw std::logic_error( "Did not get decompressed data for window!" );
            }

            /** @todo compress windows in parallel if not already compressed! */
            WindowMap::Window recompressed( *decompressed, windowCompressionType );
            std::cerr << "Write out window sized: " << decompressed->size() << " recompressed to "
                      << toString( windowCompressionType ) << " size: " << recompressed.compressedSize() << "\n";
            const auto& compressedData = recompressed.compressedData();
            if ( !compressedData ) {
                throw std::logic_error( "Did not get compressed data for window!" );
            }

            checkedWrite( compressedData->data(), compressedData->size() );
        }
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

    static constexpr size_t HEADER_BUFFER_SIZE = MAGIC_BYTES.size() + /* version */ 1U + sizeof( uint64_t );

    if ( alreadyReadBytes.size() > HEADER_BUFFER_SIZE ) {
        throw std::invalid_argument( "This function only supports skipping up to over the magic bytes if given." );
    }
    if ( alreadyReadBytes.size() != indexFile->tell() ) {
        throw std::invalid_argument( "The file position must match the number of given bytes." );
    }

    auto headerBytes = alreadyReadBytes;
    if ( headerBytes.size() < HEADER_BUFFER_SIZE ) {
        const auto oldSize = headerBytes.size();
        headerBytes.resize( HEADER_BUFFER_SIZE );
        checkedRead( indexFile.get(), headerBytes.data() + oldSize, headerBytes.size() - oldSize );
    }

    if ( !std::equal( MAGIC_BYTES.begin(), MAGIC_BYTES.end(), headerBytes.begin() ) ) {
        throw std::invalid_argument( "Magic bytes do not match!" );
    }

    const auto headerBytesReader = std::make_unique<BufferViewFileReader>( headerBytes.data(), headerBytes.size() );
    headerBytesReader->seekTo( MAGIC_BYTES.size() );
    const auto formatVersion = readValue<uint8_t>( headerBytesReader.get() );
    if ( formatVersion > 1 ) {
        throw std::invalid_argument( "Index was written with a newer rapidgzip version than supported!" );
    }

    /* Read index header. */
    GzipIndex index;
    index.compressedSizeInBytes = readValue<uint64_t>( headerBytesReader.get() );
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
[[nodiscard]] inline size_t
countDecompressedBytes( gzip::BitReader                bitReader,  // NOLINT(performance-unnecessary-value-param)
                        VectorView<std::uint8_t> const initialWindow )
{
    #ifdef LIBRAPIDARCHIVE_WITH_ISAL
        using InflateWrapper = rapidgzip::IsalInflateWrapper;
    #else
        using InflateWrapper = rapidgzip::ZlibInflateWrapper;
    #endif

    // NOLINTNEXTLINE(performance-move-const-arg)
    InflateWrapper inflateWrapper( std::move( bitReader ), std::numeric_limits<size_t>::max() );
    inflateWrapper.setWindow( initialWindow );

    size_t alreadyDecoded{ 0 };
    std::vector<uint8_t> subchunk( 128_Ki );
    while ( true ) {
        std::optional<rapidgzip::Footer> footer;
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
        index.windows->emplace( checkpoint.compressedOffsetInBits, {}, CompressionType::NONE );
    }

    try {
        gzip::BitReader bitReader( sharedArchiveFile->clone() );
        bitReader.seekTo( index.checkpoints.back().compressedOffsetInBits );
        index.uncompressedSizeInBytes = index.checkpoints.back().uncompressedOffsetInBytes
                                        // NOLINTNEXTLINE(performance-move-const-arg)
                                        + countDecompressedBytes( std::move( bitReader ), {} );
    } catch ( const std::invalid_argument& ) {
        throw std::invalid_argument( "Unable to read from the last given offset in the index!" );
    }

    return index;
}
}  // namespace bgzip


namespace indexed_gzip
{
static constexpr std::string_view MAGIC_BYTES{ "GZIDX" };


[[nodiscard]] inline GzipIndex
readGzipIndex( UniqueFileReader            indexFile,
               const std::optional<size_t> archiveSize = std::nullopt,
               const std::vector<char>&    alreadyReadBytes = {},
               size_t                      parallelization = 1 )
{
    if ( !indexFile ) {
        throw std::invalid_argument( "Index file reader must be valid!" );
    }
    if ( alreadyReadBytes.size() != indexFile->tell() ) {
        throw std::invalid_argument( "The file position must match the number of given bytes." );
    }
    static constexpr size_t HEADER_BUFFER_SIZE = MAGIC_BYTES.size() + /* version */ 1U + /* reserved flags */ 1U
                                                 + 2 * sizeof( uint64_t ) + 2 * sizeof( uint32_t );
    if ( alreadyReadBytes.size() > HEADER_BUFFER_SIZE ) {
        throw std::invalid_argument( "This function only supports skipping up to over the magic bytes if given." );
    }

    auto headerBytes = alreadyReadBytes;
    if ( headerBytes.size() < HEADER_BUFFER_SIZE ) {
        const auto oldSize = headerBytes.size();
        headerBytes.resize( HEADER_BUFFER_SIZE );
        checkedRead( indexFile.get(), headerBytes.data() + oldSize, headerBytes.size() - oldSize );
    }

    if ( !std::equal( MAGIC_BYTES.begin(), MAGIC_BYTES.end(), headerBytes.begin() ) ) {
        throw std::invalid_argument( "Magic bytes do not match! Expected 'GZIDX'." );
    }

    const auto headerBytesReader = std::make_unique<BufferViewFileReader>( headerBytes.data(), headerBytes.size() );
    headerBytesReader->seekTo( MAGIC_BYTES.size() );
    const auto formatVersion = readValue<uint8_t>( headerBytesReader.get() );
    if ( formatVersion > 1 ) {
        throw std::invalid_argument( "Index was written with a newer indexed_gzip version than supported!" );
    }

    headerBytesReader->seek( 1, SEEK_CUR );  // Skip reserved flags 1B

    GzipIndex index;
    index.compressedSizeInBytes   = readValue<uint64_t>( headerBytesReader.get() );
    index.uncompressedSizeInBytes = readValue<uint64_t>( headerBytesReader.get() );
    index.checkpointSpacing       = readValue<uint32_t>( headerBytesReader.get() );
    index.windowSizeInBytes       = readValue<uint32_t>( headerBytesReader.get() );

    indexFile->seekTo( HEADER_BUFFER_SIZE );

    if ( archiveSize && ( *archiveSize != index.compressedSizeInBytes ) ) {
        std::stringstream message;
        message << "File size for the compressed file (" << *archiveSize
                << ") does not fit the size stored in the given index (" << index.compressedSizeInBytes << ")!";
        throw std::invalid_argument( std::move( message ).str() );
    }

    /* However, a window size larger than 32 KiB makes no sense because the Lempel-Ziv back-references
     * in the deflate format are limited to 32 KiB! Smaller values might, however, be enforced by especially
     * memory-constrained encoders.
     * This basically means that we either check for this to be exactly 32 KiB or we simply throw away all
     * other data and only load the last 32 KiB of the window buffer. */
    if ( index.windowSizeInBytes != 32_Ki ) {
        throw std::invalid_argument( "Only a window size of 32 KiB makes sense because indexed_gzip supports "
                                     "no smaller ones and gzip does support any larger one." );
    }
    const auto checkpointCount = readValue<uint32_t>( indexFile.get() );

    std::vector<std::tuple</* encoded offset */ size_t, /* window size */ size_t,
                           /* compression ratio */ double> > windowInfos;

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

        auto compressionRatio = 1.0;
        if ( i >= 1 ) {
            const auto& previousCheckpoint = index.checkpoints[i - 1];
            compressionRatio = static_cast<double>( checkpoint.uncompressedOffsetInBytes
                                                    - previousCheckpoint.uncompressedOffsetInBytes ) * 8
                               / static_cast<double>( checkpoint.compressedOffsetInBits
                                                      - previousCheckpoint.compressedOffsetInBits );
        }
        windowInfos.emplace_back( checkpoint.compressedOffsetInBits, windowSize, compressionRatio );
    }

    const auto backgroundThreadCount = parallelization == 1 ? 0 : parallelization;
    ThreadPool threadPool{ backgroundThreadCount };
    std::deque<std::future<std::pair<size_t, std::shared_ptr<WindowMap::Window> > > > futures;

    /* Waits for at least one future and inserts it into the window map. */
    const auto processFuture =
        [&] ()
        {
            using namespace std::chrono_literals;

            if ( futures.empty() ) {
                return;
            }

            const auto oldSize = futures.size();
            for ( auto it = futures.begin(); it != futures.end(); ) {
                auto& future = *it;
                if ( !future.valid() || ( future.wait_for( 0s ) == std::future_status::ready ) ) {
                    auto result = future.get();
                    index.windows->emplaceShared( result.first, std::move( result.second ) );
                    it = futures.erase( it );
                } else {
                    ++it;
                }
            }

            if ( futures.size() >= oldSize ) {
                auto result = futures.front().get();
                index.windows->emplaceShared( result.first, std::move( result.second ) );
                futures.pop_front();
            }
        };

    index.windows = std::make_shared<WindowMap>();
    for ( auto& [offset, windowSize, compressionRatio] : windowInfos ) {
        /* Package the non-copyable FasterVector into a copyable smart pointer because the lambda given into the
         * ThreadPool gets inserted into a std::function living inside std::packaged_task, and std::function
         * requires every capture to be copyable. While it may compile with Clang and GCC, it does not with MSVC. */
        auto window = std::make_shared<FasterVector<uint8_t> >();
        if ( windowSize > 0 ) {
            window->resize( windowSize );
            checkedRead( indexFile.get(), window->data(), window->size() );
        }

        /* Only bother with overhead-introducing compression for large chunk compression ratios. */
        if ( compressionRatio > 2  ) {
            futures.emplace_back( threadPool.submit( [toCompress = std::move( window ), offset2 = offset] () mutable {
                return std::make_pair(
                    offset2, std::make_shared<WindowMap::Window>( std::move( *toCompress ), CompressionType::ZLIB ) );
            } ) );
            if ( futures.size() >= 2 * backgroundThreadCount ) {
                processFuture();
            }
        } else {
            index.windows->emplaceShared(
                offset, std::make_shared<WindowMap::Window>( std::move( *window ), CompressionType::NONE ) );
        }
    }

    while ( !futures.empty() ) {
        processFuture();
    }

    return index;
}


inline void
writeGzipIndex( const GzipIndex&                                              index,
                const std::function<void( const void* buffer, size_t size )>& checkedWrite )
{
    const auto writeValue = [&checkedWrite] ( auto value ) { checkedWrite( &value, sizeof( value ) ); };

    const auto& checkpoints = index.checkpoints;
    const auto windowSizeInBytes = static_cast<uint32_t>( 32_Ki );
    const auto hasValidWindow =
        [&index, windowSizeInBytes] ( const auto& checkpoint )
        {
            if ( checkpoint.compressedOffsetInBits == index.compressedSizeInBytes * 8U ) {
                /* We do not need a window for the very last offset. */
                return true;
            }
            const auto window = index.windows->get( checkpoint.compressedOffsetInBits );
            return window && ( window->empty() || ( window->decompressedSize() >= windowSizeInBytes ) );
        };

    if ( !std::all_of( checkpoints.begin(), checkpoints.end(), hasValidWindow ) ) {
        throw std::invalid_argument( "All window sizes must be at least 32 KiB or empty!" );
    }

    checkedWrite( MAGIC_BYTES.data(), MAGIC_BYTES.size() );
    checkedWrite( /* format version */ "\x01", 1 );
    checkedWrite( /* reserved flags */ "\x00", 1 );  // NOLINT(bugprone-string-literal-with-embedded-nul)

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

    writeValue( static_cast<uint64_t>( index.compressedSizeInBytes ) );
    writeValue( static_cast<uint64_t>( index.uncompressedSizeInBytes ) );
    writeValue( static_cast<uint32_t>( checkpointSpacing ) );
    writeValue( static_cast<uint32_t>( windowSizeInBytes ) );
    writeValue( static_cast<uint32_t>( checkpoints.size() ) );

    for ( const auto& checkpoint : checkpoints ) {
        const auto bits = checkpoint.compressedOffsetInBits % 8;
        writeValue( static_cast<uint64_t>( checkpoint.compressedOffsetInBits / 8 + ( bits == 0 ? 0 : 1 ) ) );
        writeValue( static_cast<uint64_t>( checkpoint.uncompressedOffsetInBytes ) );
        writeValue( static_cast<uint8_t>( bits == 0 ? 0 : 8 - bits ) );

        const auto isLastWindow = checkpoint.compressedOffsetInBits == index.compressedSizeInBytes * 8U;
        const auto result = index.windows->get( checkpoint.compressedOffsetInBits );
        if ( !result && !isLastWindow ) {
            throw std::logic_error( "Did not find window to offset " +
                                    formatBits( checkpoint.compressedOffsetInBits ) );
        }
        writeValue( static_cast<uint8_t>( !result || result->empty() ? 0 : 1 ) );
    }

    for ( const auto& checkpoint : checkpoints ) {
        const auto result = index.windows->get( checkpoint.compressedOffsetInBits );
        if ( !result ) {
            /* E.g., allowed for the checkpoint at the end of the file. */
            continue;
        }

        const auto windowPointer = result->decompress();
        if ( !windowPointer ) {
            continue;
        }

        const auto& window = *windowPointer;
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
}  // namespace indexed_gzip


namespace gztool
{
/**
 * @verbatim
 * Such an index can be created with gztool:
 *   sudo apt install gztool
 *   gztool -s 1 -z foo.gz
 *
 * Gztool Format Outline:
 *
 * Offset | Size | Value          | Description
 * -------+------+----------------+---------------------------------------------------
 *      0 |    8 | 0              | Magic Bytes for bgzip index compatibility
 * -------+------+----------------+---------------------------------------------------
 *      8 |    7 | "gzipind"      | Magic Bytes
 * -------+------+----------------+---------------------------------------------------
 *     15 |    1 | "x" or "X"     | Format version.
 *        |      |                | Version 0 ("x") does not contain line information.
 *        |      |                | Version 1 ("X") does contain line information.
 * -------+------+----------------+---------------------------------------------------
 *     16 |    4 | Line Format    | 0: \n 1: \r (Inconsistently documented in gztool!)
 *        |      |                | Only available if format version == "X".
 * -------+------+----------------+---------------------------------------------------
 *     20 |    8 | Number of      | The amount of seek points available in the index.
 *        |      | Seek Points    |
 * -------+------+----------------+---------------------------------------------------
 *     28 |    8 | Expected Seek  | This will be UINT64_MAX while the index is still
 *        |      | Points         | created, not an actual value.
 *        |      |                | This could as well have been a flag
 *        |      |                | "index complete" instead.
 * -------+------+----------------+---------------------------------------------------
 *     36 |    ? | List of Seek   | "Number of Seek Points" seek points.
 *        |      | Points         |
 * -------+------+----------------+---------------------------------------------------
 *      ? |    8 | Uncompressed   | Only available if index is complete.
 *        |      | Size           |
 * -------+------+----------------+---------------------------------------------------
 *      ? |    8 | Line Count     | Only available if format version == "X".
 *        |      |                |
 *
 * Seek Point Member
 * Offset | Size | Value          | Description
 * -------+------+----------------+---------------------------------------------------
 *      0 |    8 | Ucompressed    | Offset in the uncompressed stream in bytes.
 *        |      | Offset         |
 * -------+------+----------------+---------------------------------------------------
 *      8 |    8 | Compressed     | ceil( compressed bit offset / 8 )
 *        |      | Offset         |
 * -------+------+----------------+---------------------------------------------------
 *     16 |    4 | Compressed     | compressed bit offset
 *        |      | Offset Bits    | - mod( compressed bit offset / 8 )
 *        |      |                | (3 bits or 1 B would have been enough for this.)
 * -------+------+----------------+---------------------------------------------------
 *     24 |    4 | Compressed     |
 *        |      | Window Size    |
 * -------+------+----------------+---------------------------------------------------
 *     28 |    ? | Compressed     |
 *        |      | Window         |
 * -------+------+----------------+---------------------------------------------------
 *      ? |    8 | Line Number    | Number of newlines in all preceding uncompressed
 *        |      |                | data + 1. Only available if format version == "X".
 * @endverbatim
 *
 * The line number of the first seek point will always be 1 by definition.
 * @see https://github.com/circulosmeos/gztool/blob/d0088a3314bd7a80c1ea126de7729d0039cb5b3d/gztool.c#L3754
 * That's also why the free-standing total line number at the end of the index file is necessary to have.
 */
static constexpr std::string_view MAGIC_BYTES{ "\0\0\0\0\0\0\0\0gzipind", 8U + 7U };


[[nodiscard]] inline GzipIndex
readGzipIndex( UniqueFileReader            indexFile,
               const std::optional<size_t> archiveSize = {},
               const std::vector<char>&    alreadyReadBytes = {} )
{
    if ( !indexFile ) {
        throw std::invalid_argument( "Index file reader must be valid!" );
    }
    if ( alreadyReadBytes.size() != indexFile->tell() ) {
        throw std::invalid_argument( "The file position must match the number of given bytes." );
    }
    static constexpr size_t HEADER_BUFFER_SIZE = MAGIC_BYTES.size() + 1U;
    if ( alreadyReadBytes.size() > HEADER_BUFFER_SIZE ) {
        throw std::invalid_argument( "This function only supports skipping up to over the magic bytes if given." );
    }

    GzipIndex index;

    /* We need a seekable archive to add the very first and very last offset pairs.
     * If the archive is not seekable, loading the index makes not much sense anyways.
     * If it is still needed, then use a better index file format instead of gztool index. */
    if ( !archiveSize ) {
        throw std::invalid_argument( "Cannot import gztool index without knowing the archive size!" );
    }
    index.compressedSizeInBytes = archiveSize.value();

    auto headerBytes = alreadyReadBytes;
    if ( headerBytes.size() < HEADER_BUFFER_SIZE ) {
        const auto oldSize = headerBytes.size();
        headerBytes.resize( HEADER_BUFFER_SIZE );
        checkedRead( indexFile.get(), headerBytes.data() + oldSize, headerBytes.size() - oldSize );
    }

    if ( !std::equal( MAGIC_BYTES.begin(), MAGIC_BYTES.end(), headerBytes.begin() ) ) {
        throw std::invalid_argument( "Magic bytes do not match!" );
    }

    if ( ( headerBytes.back() != 'x' ) && ( headerBytes.back() != 'X' ) ) {
        throw std::invalid_argument( "Invalid index version. Expected 'x' or 'X'!" );
    }
    const auto formatVersion = headerBytes.back() == 'x' ? 0 : 1;
    if ( formatVersion > 1 ) {
        throw std::invalid_argument( "Index was written with a newer indexed_gzip version than supported!" );
    }

    index.hasLineOffsets = formatVersion == 1;
    if ( index.hasLineOffsets ) {
        const auto format = readBigEndianValue<uint32_t>( indexFile.get() );
        if ( format > 1 ) {
            throw std::invalid_argument( "Expected 0 or 1 for newline format!" );
        }
        index.newlineFormat = format == 0 ? NewlineFormat::LINE_FEED : NewlineFormat::CARRIAGE_RETURN;
    }

    const auto checkpointCount = readBigEndianValue<uint64_t>( indexFile.get() );
    const auto indexIsComplete = checkpointCount == readBigEndianValue<uint64_t>( indexFile.get() );
    if ( !indexIsComplete ) {
        throw std::invalid_argument( "Reading an incomplete index is not supported!" );
    }

    index.windows = std::make_shared<WindowMap>();

    std::array<uint8_t, rapidgzip::deflate::MAX_WINDOW_SIZE> decompressedWindow{};

    index.checkpoints.resize( checkpointCount );
    for ( uint32_t i = 0; i < checkpointCount; ++i ) {
        auto& checkpoint = index.checkpoints[i];

        checkpoint.uncompressedOffsetInBytes = readBigEndianValue<uint64_t>( indexFile.get() );
        if ( checkpoint.uncompressedOffsetInBytes > index.uncompressedSizeInBytes ) {
            throw std::invalid_argument( "Checkpoint uncompressed offset is after the file end!" );
        }

        /* First load only compressed offset rounded down in bytes, the bits are loaded down below! */
        checkpoint.compressedOffsetInBits = readBigEndianValue<uint64_t>( indexFile.get() );
        if ( checkpoint.compressedOffsetInBits > index.compressedSizeInBytes ) {
            throw std::invalid_argument( "Checkpoint compressed offset is after the file end!" );
        }
        checkpoint.compressedOffsetInBits *= 8;

        const auto bits = readBigEndianValue<uint32_t>( indexFile.get() );
        if ( bits >= 8 ) {
            throw std::invalid_argument( "Denormal compressed offset for checkpoint. Bit offset >= 8!" );
        }
        if ( bits > 0 ) {
            if ( checkpoint.compressedOffsetInBits == 0 ) {
                throw std::invalid_argument( "Denormal bits for checkpoint. Effectively negative offset!" );
            }
            checkpoint.compressedOffsetInBits -= bits;
        }

        const auto compressedWindowSize = readBigEndianValue<uint32_t>( indexFile.get() );
        if ( compressedWindowSize == 0 ) {
            /* Emplace an empty window to show that the chunk does not need data. */
            index.windows->emplace( checkpoint.compressedOffsetInBits, {}, CompressionType::NONE );
        } else {
            FasterVector<uint8_t> compressedWindow( compressedWindowSize );
            checkedRead( indexFile.get(), compressedWindow.data(), compressedWindow.size() );

            /** @todo Parallelize or avoid decompression just in order to find out the decompressed size.
             *        Simply defining a new more suitable format seems easier. */
            gzip::BitReader bitReader(
                std::make_unique<BufferViewFileReader>( compressedWindow.data(), compressedWindow.size() ) );

        #ifdef LIBRAPIDARCHIVE_WITH_ISAL
            using InflateWrapper = rapidgzip::IsalInflateWrapper;
        #else
            using InflateWrapper = rapidgzip::ZlibInflateWrapper;
        #endif

            InflateWrapper inflateWrapper( std::move( bitReader ) );
            inflateWrapper.setFileType( rapidgzip::FileType::ZLIB );
            inflateWrapper.setStartWithHeader( true );

            const auto [decompressedWindowSize, footer] = inflateWrapper.readStream( decompressedWindow.data(),
                                                                                     decompressedWindow.size() );
            if ( !footer ) {
                throw std::invalid_argument( "Expected zlib footer after at least 32 KiB of data!" );
            }

            index.windows->emplaceShared( checkpoint.compressedOffsetInBits,
                                          std::make_shared<WindowMap::Window>( std::move( compressedWindow ),
                                                                               decompressedWindowSize,
                                                                               CompressionType::ZLIB ) );
        }

        if ( index.hasLineOffsets ) {
            checkpoint.lineOffset = readBigEndianValue<uint64_t>( indexFile.get() );
            if ( checkpoint.lineOffset == 0 ) {
                throw std::invalid_argument( "Line number in gztool index is expected to be >0 by definition!" );
            }
            checkpoint.lineOffset -= 1;
        }
    }

    if ( indexIsComplete ) {
        index.uncompressedSizeInBytes = readBigEndianValue<uint64_t>( indexFile.get() );
        if ( index.hasLineOffsets ) {
            if ( index.checkpoints.empty()
                 || ( index.checkpoints.back().compressedOffsetInBits != index.compressedSizeInBytes * 8U ) )
            {
                auto& checkpoint = index.checkpoints.emplace_back();
                checkpoint.compressedOffsetInBits = index.compressedSizeInBytes * 8U;
                checkpoint.uncompressedOffsetInBytes = index.uncompressedSizeInBytes;

                /* Emplace an empty window to show that the chunk at the file end does not need data. */
                index.windows->emplace( checkpoint.compressedOffsetInBits, {}, CompressionType::NONE );
            } else if ( index.checkpoints.back().uncompressedOffsetInBytes != index.uncompressedSizeInBytes ) {
                throw std::domain_error( "The last checkpoint at the end of the compressed stream does not match "
                                         "the uncompressed size!" );
            }
            index.checkpoints.back().lineOffset = readBigEndianValue<uint64_t>( indexFile.get() );
        }
    }

    return index;
}


inline void
writeGzipIndex( const GzipIndex&                                              index,
                const std::function<void( const void* buffer, size_t size )>& checkedWrite )
{
    const auto writeValue = [&checkedWrite] ( auto value ) {
        if ( ENDIAN == Endian::BIG ) {
            checkedWrite( &value, sizeof( value ) );
        } else {
            std::array<char, sizeof( value )> buffer{};
            auto* const src = reinterpret_cast<char*>( &value );
            for ( size_t i = 0; i < sizeof( value ); ++i ) {
                buffer[buffer.size() - 1 - i] = src[i];
            }
            checkedWrite( buffer.data(), buffer.size() );
        }
    };

    const auto& checkpoints = index.checkpoints;
    const auto windowSizeInBytes = static_cast<uint32_t>( 32_Ki );
    const auto hasValidWindow =
        [&index, windowSizeInBytes] ( const auto& checkpoint )
        {
            if ( checkpoint.compressedOffsetInBits == index.compressedSizeInBytes * 8U ) {
                /* We do not need a window for the very last offset. */
                return true;
            }
            const auto window = index.windows->get( checkpoint.compressedOffsetInBits );
            return window && ( window->empty() || ( window->decompressedSize() >= windowSizeInBytes ) );
        };

    if ( !std::all_of( checkpoints.begin(), checkpoints.end(), hasValidWindow ) ) {
        throw std::invalid_argument( "All window sizes must be at least 32 KiB or empty!" );
    }

    checkedWrite( MAGIC_BYTES.data(), MAGIC_BYTES.size() );
    checkedWrite( /* format version */ index.hasLineOffsets ? "X" : "x", 1 );
    if ( index.hasLineOffsets ) {
        writeValue( static_cast<uint32_t>( index.newlineFormat == NewlineFormat::LINE_FEED ? 0 : 1 ) );
    }

    /* Do not write out the last checkpoint at the end of the file because gztool also does not write those. */
    auto lastCheckPoint = index.checkpoints.rbegin();
    while ( ( lastCheckPoint != index.checkpoints.rend() )
            && ( lastCheckPoint->uncompressedOffsetInBytes == index.uncompressedSizeInBytes ) )
    {
        ++lastCheckPoint;
    }
    const auto checkpointCount = index.checkpoints.size() - std::distance( index.checkpoints.rbegin(), lastCheckPoint );
    writeValue( /* Number of Seek Points */ static_cast<uint64_t>( checkpointCount ) );
    writeValue( /* Number of Expected Seek Points */ static_cast<uint64_t>( checkpointCount ) );

    for ( const auto& checkpoint : checkpoints ) {
        if ( checkpoint.compressedOffsetInBits == index.compressedSizeInBytes * 8U ) {
            continue;
        }

        const auto bits = checkpoint.compressedOffsetInBits % 8;
        writeValue( static_cast<uint64_t>( checkpoint.uncompressedOffsetInBytes ) );
        writeValue( static_cast<uint64_t>( checkpoint.compressedOffsetInBits / 8 + ( bits == 0 ? 0 : 1 ) ) );
        writeValue( static_cast<uint32_t>( bits == 0 ? 0 : 8 - bits ) );

        const auto result = index.windows->get( checkpoint.compressedOffsetInBits );
        if ( !result ) {
            throw std::logic_error( "Did not find window to offset " +
                                    formatBits( checkpoint.compressedOffsetInBits ) );
        }
        if ( result->empty() ) {
            writeValue( uint32_t( 0 ) );
        } else if ( result->compressionType() == CompressionType::ZLIB ) {
            writeValue( static_cast<uint32_t>( result->compressedSize() ) );
            const auto compressedData = result->compressedData();
            if ( !compressedData ) {
                throw std::logic_error( "Did not get compressed data buffer!" );
            }
            checkedWrite( compressedData->data(), compressedData->size() );
        } else {
            /* Recompress window to ZLIB. */
            /**
             * @todo Reduce overhead from the usual gzip data by stripping of the gzip container and re-adding
             *       a zlib container. This can keep the byte-aligned deflate stream but will require decompressing
             *       it in order to compute the Adler32 checksum for the zlib footer.
             */
            const auto windowPointer = result->decompress();
            if ( !windowPointer ) {
                throw std::logic_error( "Did not get decompressed data buffer!" );
            }

            const auto& window = *windowPointer;
            if ( window.empty() ) {
                continue;
            }

            using namespace rapidgzip;
            const auto recompressed = compressWithZlib( window, CompressionStrategy::DEFAULT, /* dictionary */ {},
                                                        ContainerFormat::ZLIB );
            writeValue( static_cast<uint32_t>( recompressed.size() ) );
            checkedWrite( recompressed.data(), recompressed.size() );
        }

        if ( index.hasLineOffsets ) {
            writeValue( static_cast<uint64_t>( checkpoint.lineOffset + 1U /* gztool starts counting from 1 */ ) );
        }
    }

    writeValue( static_cast<uint64_t>( index.uncompressedSizeInBytes ) );
    if ( index.hasLineOffsets ) {
        writeValue( static_cast<uint64_t>( index.checkpoints.empty() ? 0 : index.checkpoints.rbegin()->lineOffset ) );
    }
}
}  // namespace gztool


[[nodiscard]] inline GzipIndex
readGzipIndex( UniqueFileReader indexFile,
               UniqueFileReader archiveFile = {},
               size_t           parallelization = 1 )
{
    std::vector<char> formatId( 8, 0 );
    checkedRead( indexFile.get(), formatId.data(), formatId.size() );

    std::optional<size_t> archiveSize;
    if ( archiveFile ) {
        archiveSize = archiveFile->size();
    }

    if ( const auto commonSize = std::min( formatId.size(), RandomAccessIndex::MAGIC_BYTES.size() );
         std::string_view( formatId.data(), commonSize )
         == std::string_view( RandomAccessIndex::MAGIC_BYTES.data(), commonSize ) )
    {
        return RandomAccessIndex::readGzipIndex( std::move( indexFile ), archiveSize, formatId );
    }

    if ( const auto commonSize = std::min( formatId.size(), indexed_gzip::MAGIC_BYTES.size() );
         std::string_view( formatId.data(), commonSize )
         == std::string_view( indexed_gzip::MAGIC_BYTES.data(), commonSize ) )
    {
        return indexed_gzip::readGzipIndex( std::move( indexFile ), archiveSize, formatId, parallelization );
    }

    /* The gztool index has chosen its first 8 bytes to look just like an empty bgzip index. */
    if ( const auto commonSize = std::min( formatId.size(), gztool::MAGIC_BYTES.size() );
         std::string_view( formatId.data(), commonSize )
         == std::string_view( gztool::MAGIC_BYTES.data(), commonSize ) )
    {
        return gztool::readGzipIndex( std::move( indexFile ), archiveSize, formatId );
    }

    /* Bgzip indexes have no magic bytes and simply start with the number of chunks. */
    return bgzip::readGzipIndex( std::move( indexFile ), std::move( archiveFile ), formatId );
}
}  // namespace rapidgzip
