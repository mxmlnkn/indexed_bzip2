/*
DEFLATE Compressed Data Format Specification version 1.3
https://www.rfc-editor.org/rfc/rfc1951.txt

GZIP file format specification version 4.3
https://www.ietf.org/rfc/rfc1952.txt
*/


#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <zlib.h>

#include <core/BitManipulation.hpp>
#include <core/common.hpp>
#include <core/DataGenerators.hpp>
#include <core/Statistics.hpp>
#include <core/TestHelpers.hpp>
#include <filereader/Buffered.hpp>
#include <filereader/Standard.hpp>
#include <rapidgzip/blockfinder/Bgzf.hpp>
#include <rapidgzip/blockfinder/DynamicHuffman.hpp>
#include <rapidgzip/blockfinder/precodecheck/SingleCompressedLUT.hpp>
#include <rapidgzip/blockfinder/precodecheck/SingleLUT.hpp>
#include <rapidgzip/blockfinder/precodecheck/WalkTreeCompressedLUT.hpp>
#include <rapidgzip/blockfinder/precodecheck/WalkTreeCompressedSingleLUT.hpp>
#include <rapidgzip/blockfinder/precodecheck/WalkTreeLUT.hpp>
#include <rapidgzip/blockfinder/precodecheck/WithoutLUT.hpp>
#include <rapidgzip/gzip/crc32.hpp>
#include <rapidgzip/gzip/definitions.hpp>
#include <rapidgzip/gzip/precode.hpp>
#include <rapidgzip/huffman/HuffmanCodingCheckOnly.hpp>


using namespace rapidgzip;


std::ostream&
operator<<( std::ostream&              out,
            const std::vector<size_t>& vector )
{
    constexpr size_t MAX_VALUES_TO_PRINT = 15;
    for ( size_t i = 0; i < std::min( vector.size(), MAX_VALUES_TO_PRINT ); ++i ) {
        out << " " << vector[i];
    }
    if ( vector.size() > MAX_VALUES_TO_PRINT ) {
        out << " ...";
    }
    return out;
}


[[nodiscard]] std::vector<size_t>
findGzipStreams( const std::string& fileName )
{
    const auto file = throwingOpen( fileName, "rb" );

    static constexpr auto bufferSize = 4_Mi;
    std::vector<char> buffer( bufferSize, 0 );

    std::vector<size_t> streamOffsets;
    size_t totalBytesRead = 0;
    while ( true )
    {
        const auto bytesRead = fread( buffer.data(), sizeof( char ), buffer.size(), file.get() );
        if ( bytesRead == 0 ) {
            break;
        }

        for ( size_t i = 0; i + 8 < bytesRead; ++i ) {
            if ( ( buffer[i + 0] == (char)0x1F )
                 && ( buffer[i + 1] == (char)0x8B )
                 && ( buffer[i + 2] == (char)0x08 )
                 && ( buffer[i + 3] == (char)0x04 )
                 && ( buffer[i + 4] == (char)0x00 )  // this is assuming the mtime is zero, which obviously can differ!
                 && ( buffer[i + 5] == (char)0x00 )
                 && ( buffer[i + 6] == (char)0x00 )
                 && ( buffer[i + 7] == (char)0x00 )
                 && ( buffer[i + 8] == (char)0x00 ) ) {
                //std::cerr << "Found possible candidate for a gzip stream at offset: " << totalBytesRead + i << " B\n";
                streamOffsets.push_back( totalBytesRead + i );
            }
        }

        totalBytesRead += bytesRead;
    }

    return streamOffsets;
}


[[nodiscard]] std::vector<size_t>
findBgzStreams( const std::string& fileName )
{
    std::vector<size_t> streamOffsets;

    try {
        rapidgzip::blockfinder::Bgzf blockFinder( std::make_unique<StandardFileReader>( fileName ) );

        while ( true ) {
            const auto offset = blockFinder.find();
            if ( offset == std::numeric_limits<size_t>::max() ) {
                break;
            }
            streamOffsets.push_back( offset );
        }
    }
    catch ( const std::invalid_argument& ) {
        return {};
    }

    return streamOffsets;
}


/**
 * @see https://github.com/madler/zlib/blob/master/examples/zran.c
 */
[[nodiscard]] std::pair<std::vector<size_t>, std::vector<size_t> >
parseWithZlib( const std::string& fileName )
{
    const auto file = throwingOpen( fileName, "rb" );

    std::vector<size_t> streamOffsets;
    std::vector<size_t> blockOffsets;

    static constexpr auto BUFFER_SIZE = 1_Mi;
    static constexpr auto WINDOW_SIZE = 32_Ki;

    /**
     * Make one entire pass through the compressed stream and build an index, with
     * access points about every span bytes of uncompressed output -- span is
     * chosen to balance the speed of random access against the memory requirements
     * of the list, about 32K bytes per access point.  Note that data after the end
     * of the first zlib or gzip stream in the file is ignored.  build_index()
     * returns the number of access points on success (>= 1), Z_MEM_ERROR for out
     * of memory, Z_DATA_ERROR for an error in the input file, or Z_ERRNO for a
     * file read error.  On success, *built points to the resulting index.
     */
    std::array<unsigned char, BUFFER_SIZE> input{};
    std::array<unsigned char, WINDOW_SIZE> window{};

    /* initialize inflate */
    z_stream stream;
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;
    stream.avail_in = 0;
    stream.next_in = Z_NULL;

    const auto throwCode = [] ( const auto code ) { throw std::domain_error( std::to_string( code ) ); };

    /* Second argument is window bits. log2 base of window size. Adding 32 to that (setting the 5-th bit),
     * means that automatic zlib or gzip decoding is detected. */
    auto ret = inflateInit2( &stream, 32 + 15 );
    if ( ret != Z_OK ) {
        throwCode( ret );
    }

    std::vector<unsigned char> extraBuffer( 1_Ki );

    gz_header header;
    header.extra = extraBuffer.data();
    header.extra_max = extraBuffer.size();
    header.name = Z_NULL;
    header.comment = Z_NULL;
    header.done = 0;

    bool readHeader = true;
    ret = inflateGetHeader( &stream, &header );
    if ( ret != Z_OK ) {
        throwCode( ret );
    }
    streamOffsets.push_back( 0 );

    /* Counters to avoid 4GB limit */
    off_t totin = 0;
    stream.avail_out = 0;

    /* inflate the input, maintain a sliding window, and build an index -- this
       also validates the integrity of the compressed data using the check
       information at the end of the gzip or zlib stream */
    while ( true )
    {
        /* get some compressed data from input file */
        stream.avail_in = std::fread( input.data(), 1, input.size(), file.get() );
        if ( ( stream.avail_in == 0 ) && ( std::feof( file.get() ) != 0 ) ) {
            break;
        }
        if ( std::ferror( file.get() ) != 0 ) {
            throwCode( Z_ERRNO );
        }
        if ( stream.avail_in == 0 ) {
            throwCode( Z_DATA_ERROR );
        }
        stream.next_in = input.data();

        /* process all of that, or until end of stream */
        while ( stream.avail_in != 0 )
        {
            /* reset sliding window if necessary */
            if ( stream.avail_out == 0 ) {
                stream.avail_out = window.size();
                stream.next_out = window.data();
            }

            /* inflate until out of input, output, or at end of block --
               update the total input and output counters */
            totin  += stream.avail_in;
            ret     = inflate( &stream, Z_BLOCK );  /* return at end of block */
            totin  -= stream.avail_in;
            if ( ret == Z_NEED_DICT ) {
                ret = Z_DATA_ERROR;
            }
            if ( ( ret == Z_MEM_ERROR ) || ( ret == Z_DATA_ERROR ) ) {
                throwCode( ret );
            }

            if ( readHeader && ( header.done == 1 ) && ( header.extra_len > 0 ) ) {
                readHeader = false;
                /* retry if extra did not fit? */
                extraBuffer.resize( std::min( header.extra_len, static_cast<unsigned int>( extraBuffer.size() ) ) );
                std::cout << "Got " << extraBuffer.size() << " B of FEXTRA field!\n";
            }

            if ( ret == Z_STREAM_END ) {
                ret = inflateReset( &stream );
                if ( ret == Z_OK ) {
                    streamOffsets.push_back( totin );
                }
                continue;
            }

            /**
             * > The Z_BLOCK option assists in appending to or combining deflate streams.
             * > To assist in this, on return inflate() always sets strm->data_type to the
             * > number of unused bits in the last byte taken from strm->next_in, plus 64 if
             * > inflate() is currently decoding the last block in the deflate stream, plus
             * > 128 if inflate() returned immediately after decoding an end-of-block code or
             * > decoding the complete header up to just before the first byte of the deflate
             * > stream.  The end-of-block will not be indicated until all of the uncompressed
             * > data from that block has been written to strm->next_out.  The number of
             * > unused bits may in general be greater than seven, except when bit 7 of
             * > data_type is set, in which case the number of unused bits will be less than
             * > eight.  data_type is set as noted here every time inflate() returns for all
             * > flush options, and so can be used to determine the amount of currently
             * > consumed input in bits.
             * -> bit 7 corresponds to 128 -> if set, then number of unused bits is less than 8 -> therefore &7!
             *    as zlib stops AFTER the block, we are not interested in the offset for the last block,
             *    i.e., we check against the 6-th bit, which corresponds to ( x & 64 ) == 0 for all but last block.
             */
            const auto bits = static_cast<std::make_unsigned_t<decltype( stream.data_type )> >( stream.data_type );
            if ( ( ( bits & 128U ) != 0 ) && ( ( bits & 64U ) == 0 ) ) {
                blockOffsets.push_back( totin * 8U - ( bits & 7U ) );
            }
        }
    }

    /* clean up and return index (release unused entries in list) */
    (void) inflateEnd( &stream );
    return { streamOffsets, blockOffsets };
}


class GzipWrapper
{
public:
    static constexpr auto WINDOW_SIZE = 32_Ki;

    enum class Format
    {
        AUTO,
        RAW,
        GZIP,
    };

public:
    explicit
    GzipWrapper( Format format = Format::AUTO )
    {
        m_stream.zalloc = Z_NULL;     /* used to allocate the internal state */
        m_stream.zfree = Z_NULL;      /* used to free the internal state */
        m_stream.opaque = Z_NULL;     /* private data object passed to zalloc and zfree */

        m_stream.avail_in = 0;        /* number of bytes available at next_in */
        m_stream.next_in = Z_NULL;    /* next input byte */

        m_stream.avail_out = 0;       /* remaining free space at next_out */
        m_stream.next_out = Z_NULL;   /* next output byte will go here */

        m_stream.msg = nullptr;

        int windowBits = 15;  // maximum value corresponding to 32kiB;
        switch ( format )
        {
        case Format::AUTO:
            windowBits += 32;
            break;

        case Format::RAW:
            windowBits *= -1;
            break;

        case Format::GZIP:
            windowBits += 16;
            break;
        }

        auto ret = inflateInit2( &m_stream, windowBits );
        if ( ret != Z_OK ) {
            throw std::domain_error( std::to_string( ret ) );
        }
    }

    GzipWrapper( const GzipWrapper& ) = delete;

    GzipWrapper( GzipWrapper&& ) = delete;

    GzipWrapper&
    operator=( GzipWrapper&& ) = delete;

    GzipWrapper&
    operator=( GzipWrapper& ) = delete;

    ~GzipWrapper()
    {
        inflateEnd( &m_stream );
    }

    bool
    tryInflate( unsigned char const* compressed,
                size_t               compressedSize,
                size_t               bitOffset = 0 )
    {
        if ( inflateReset( &m_stream ) != Z_OK ) {
            return false;
        }

        if ( ceilDiv( bitOffset, CHAR_BIT ) >= compressedSize ) {
            return false;
        }

        const auto bitsToSeek = bitOffset % CHAR_BIT;
        const auto byteOffset = bitOffset / CHAR_BIT;
        m_stream.avail_in = compressedSize - byteOffset;
        /* const_cast should be safe because zlib presumably only uses this in a const manner.
         * I'll probably have to roll out my own deflate decoder anyway so I might be able
         * to change this bothersome interface. */
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        m_stream.next_in = const_cast<unsigned char*>( compressed ) + byteOffset;

        const auto outputPreviouslyAvailable = std::min<size_t>( 8_Ki, m_outputBuffer.size() );
        m_stream.avail_out = outputPreviouslyAvailable;
        m_stream.next_out = m_outputBuffer.data();

        /* Using std::fill leads to 10x slowdown -.-!!! Memset probably better.
         * Well, or not necessary at all because we are not interested in the specific output values anyway.
         * std::memset only incurs a 30% slowdown. */
        //std::fill( m_window.begin(), m_window.end(), '\0' );
        //std::memset( m_window.data(), 0, m_window.size() );
        if ( bitsToSeek > 0 ) {
            m_stream.next_in += 1;
            m_stream.avail_in -= 1;

            auto errorCode = inflatePrime( &m_stream, static_cast<int>( 8U - bitsToSeek ),
                                           compressed[byteOffset] >> bitsToSeek );
            if ( errorCode != Z_OK ) {
                return false;
            }
        }

        auto errorCode = inflateSetDictionary( &m_stream, m_window.data(), m_window.size() );
        if ( errorCode != Z_OK ) {}

        errorCode = inflate( &m_stream, Z_BLOCK );
        if ( ( errorCode != Z_OK ) && ( errorCode != Z_STREAM_END ) ) {
            return false;
        }

        if ( errorCode == Z_STREAM_END ) {
            /* We are not interested in blocks close to the stream end.
             * Because either this is close to the end and no parallelization is necessary,
             * or this means the gzip file is compromised of many gzip streams, which are a tad
             * easier to search for than raw deflate streams! */
            return false;
        }
        const auto nBytesDecoded = outputPreviouslyAvailable - m_stream.avail_out;
        return nBytesDecoded >= outputPreviouslyAvailable;
    }

private:
    z_stream m_stream{};
    std::vector<unsigned char> m_window = std::vector<unsigned char>( 32_Ki, '\0' );
    std::vector<unsigned char> m_outputBuffer = std::vector<unsigned char>( 64_Mi );
};


[[nodiscard]] std::vector<size_t>
findDeflateBlocksZlib( BufferedFileReader::AlignedBuffer buffer )
{
    std::vector<size_t> bitOffsets;
    GzipWrapper gzip( GzipWrapper::Format::RAW );

    for ( size_t offset = 0; offset <= ( buffer.size() - 1 ) * sizeof( buffer[0] ) * CHAR_BIT; ++offset ) {
        if ( gzip.tryInflate( reinterpret_cast<unsigned char const*>( buffer.data() ),
                              buffer.size() * sizeof( buffer[0] ),
                              offset ) ) {
            bitOffsets.push_back( offset );
        }
    }
    return bitOffsets;
}


[[nodiscard]] std::vector<size_t>
findDeflateBlocksZlibOptimized( BufferedFileReader::AlignedBuffer buffer )
{
    gzip::BitReader bitReader( std::make_unique<BufferedFileReader>( buffer ) );

    /**
     * Deflate Block:
     *
     *   Each block of compressed data begins with 3 header bits
     *   containing the following data:
     *
     *      first bit       BFINAL
     *      next 2 bits     BTYPE
     *
     *   Note that the header bits do not necessarily begin on a byte
     *   boundary, since a block does not necessarily occupy an integral
     *   number of bytes.
     *
     *   BFINAL is set if and only if this is the last block of the data
     *   set.
     *
     *   BTYPE specifies how the data are compressed, as follows:
     *
     *      00 - no compression
     *      01 - compressed with fixed Huffman codes
     *      10 - compressed with dynamic Huffman codes
     *      11 - reserved (error)
     *
     * => For a perfect compression, we wouldn't be able to find the blocks in any way because all input data
     *    would be valid data. Therefore, in order to find blocks we are trying to find and make use of any
     *    kind of redundancy / invalid values, which might appear.
     * -> We can reduce the number of bit offsets to try by assuming BFINAL = 0,
     *    which should not matter for performance anyway. This is a kind of redundancy, which could have been
     *    compressed further by saving the number of expected blocks at the beginning. This number would amortize
     *    after 64 blocks for a 64-bit number. And it could even be stored more compactly like done in UTF-8.
     */

    /**
     * @verbatim
     *         GZM CMP FLG   MTIME    XFL OS      FNAME
     *        <---> <> <> <--------->  <> <> <----------------
     * @0x00  1f 8b 08 08 bb 97 d7 61  02 03 74 69 6e 79 62 36  |.......a..tinyb6|
     *
     *        FNAME Blocks starting at 18 B
     *        <---> <----
     * @0x10  34 00 14 9d b7 7a 9c 50  10 46 7b bd 0a 05 2c 79  |4....z.P.F{...,y|
     * @0x20  4b 72 5a 72 a6 23 e7 9c  79 7a e3 c6 85 3e 5b da  |KrZr.#..yz...>[.|
     *        <--------------------->
     *               uint64_t
     * @endverbatim
     */

    auto* const cBuffer = reinterpret_cast<unsigned char*>( buffer.data() );

    std::vector<size_t> bitOffsets;
    GzipWrapper gzip( GzipWrapper::Format::RAW );
    //size_t zlibTestCount = 0;

    uint32_t nextThreeBits = bitReader.read<2>();

    for ( size_t offset = 0; offset <= ( buffer.size() - 1 ) * sizeof( buffer[0] ) * CHAR_BIT; ++offset ) {
        nextThreeBits >>= 1U;
        nextThreeBits |= bitReader.read<1>() << 2U;

        /* Ignore final blocks and those with invalid compression. */
        /* Comment out to also find deflate blocks with bgz. But this alone reduces performance by factor 2!!!
         * Bgz will use another format anyway, so there should be no harm in skipping these. */
        if ( ( nextThreeBits & 0b001ULL ) != 0 ) {
            continue;
        }

        /* Filter out reserved block compression type. */
        if ( ( nextThreeBits & 0b110ULL ) == 0b110ULL ) {
            continue;
        }

        #if 1
        /* Check for uncompressed blocks. */
        if ( ( ( nextThreeBits >> 1U ) & 0b11ULL ) == 0b000ULL ) {
            /* Do not use CHAR_BIT because this is a deflate constant defining a byte as 8 bits. */
            const auto nextByteOffset = ceilDiv( offset + 3, 8U );
            const auto length = static_cast<uint16_t>(
                ( static_cast<uint16_t>( cBuffer[nextByteOffset + 1] ) << static_cast<uint8_t>( CHAR_BIT ) )
                + cBuffer[nextByteOffset] );
            const auto negatedLength = static_cast<uint16_t>(
                ( static_cast<uint16_t>( cBuffer[nextByteOffset + 3] )
                  << static_cast<uint8_t>( CHAR_BIT ) )
                + cBuffer[nextByteOffset + 2] );
            if ( ( length != static_cast<uint16_t>( ~negatedLength ) ) || ( length < 8_Ki ) ) {
                continue;
            }

            /** @todo check if padded bits are zero and if so, then mark all of them belonging to the same block
             *        as bit offset candidates. */
            /* Note that calling zlib on this, will do not much at all, except unnecessarily copy the bytes
             * and check the size. We can check the size ourselves. Instead, we should call zlib to try and
             * decompress the next block because uncompressed block headers have comparably fewer redundancy
             * to check against! */
            const auto nextBlockOffset = nextByteOffset + 4 + length;
            /**
             * If we can't check the next block, then for now simply do not filter it.
             * @todo keep a sliding window which can keep enough buffers, i.e., ~2 * 32kiB
             *       (32kiB is largest uncompressed block length)
             */
            if ( ( nextBlockOffset < buffer.size() * sizeof( buffer[0] ) )
                 && !gzip.tryInflate( cBuffer,
                                      buffer.size() * sizeof( buffer[0] ),
                                      ( nextByteOffset + 4 + length ) * 8U ) ) {
                continue;
            }

            bitOffsets.push_back( offset );
            continue;
        }
        #endif

        /**
         * Note that stored blocks begin with 0b000 and furthermore the next value is padded to byte areas.
         * This means that we can't say for certain at which bit offset the block begins because multiple
         * can be valid because of the padding. This becomes important when matching the previous block's
         * end to this block's beginning. It would require a min,max possible range (<8)!
         */
        //++zlibTestCount;
        if ( gzip.tryInflate( reinterpret_cast<unsigned char const*>( buffer.data() ),
                              buffer.size() * sizeof( buffer[0] ),
                              offset ) ) {
            bitOffsets.push_back( offset );
        }
    }

    //const auto totalBitOffsets = ( buffer.size() - 1 ) * sizeof( buffer[0] ) * CHAR_BIT;
    //std::cout << "[findDeflateBlocksZlibOptimized] Needed to test with zlib " << zlibTestCount << " out of "
    //          << totalBitOffsets << " times\n";

    return bitOffsets;
}


[[nodiscard]] std::vector<size_t>
findDeflateBlocksRapidgzip( BufferedFileReader::AlignedBuffer buffer )
{
    using DeflateBlock = rapidgzip::deflate::Block<>;

    const auto nBitsToTest = buffer.size() * CHAR_BIT;
    gzip::BitReader bitReader( std::make_unique<BufferedFileReader>( std::move( buffer ) ) );

    std::vector<size_t> bitOffsets;

    rapidgzip::deflate::Block block;
    for ( size_t offset = 0; offset <= nBitsToTest; ++offset ) {
        bitReader.seek( static_cast<long long int>( offset ) );
        try
        {
        #if 0
            /* Unfortunately, this version with peek is ~5% slower */
            const auto isLastBlock = bitReader.peek<1>() != 0;
            if ( isLastBlock ) {
                bitReader.seekAfterPeek( 1 );
                continue;
            }
            auto error = block.readHeader( bitReader );
        #else
            auto error = block.readHeader</* count last block as error */ true>( bitReader );
        #endif
            if ( error != rapidgzip::Error::NONE ) {
                continue;
            }

            /* Ignoring fixed Huffman compressed blocks speeds up finding blocks by more than 3x!
             * This is probably because there is very few metadata to check in this case and it begins
             * decoding immediately, which has a much rarer error rate on random data. Fixed Huffman
             * is used by GNU gzip for highly compressible (all zeros) or very short data.
             * However, because of this reason, this compression type should be rather rare!
             * Because such blocks are also often only several dozens of bytes large. So, for all of the
             * blocks in 10MiB of data to use fixed Huffman coding, the encoder is either not finished yet
             * and simply can't encode dynamic Huffman blocks or we have a LOT of highly compressible data,
             * to be specific 10 GiB of uncompressed data because of the maximum compression ratio of 1032.
             * @see https://stackoverflow.com/questions/16792189/gzip-compression-ratio-for-zeros/16794960#16794960 */
            if ( block.compressionType() == DeflateBlock::CompressionType::FIXED_HUFFMAN ) {
                continue;
            }

            if ( block.compressionType() == DeflateBlock::CompressionType::UNCOMPRESSED ) {
                /* Ignore uncompressed blocks for comparability with the version using a LUT. */
                //std::cerr << "Uncompressed block candidate: " << offset << "\n";
                continue;
            }

            /* Testing decoding is not necessary because the Huffman canonical check is already very strong!
             * Decoding up to 8 kiB like in pugz only impedes performance and it is harder to reuse that already
             * decoded data if we do decide that it is a valid block. The number of checks during reading is also
             * pretty few because there almost are no wasted / invalid symbols. */
            bitOffsets.push_back( offset );
        } catch ( const gzip::BitReader::EndOfFileReached& ) {
            break;
        }
    }
    return bitOffsets;
}


template<uint8_t CACHED_BIT_COUNT>
[[nodiscard]] uint64_t
countDeflateBlocksPreselection( BufferedFileReader::AlignedBuffer data )
{
    const size_t nBitsToTest = data.size() * CHAR_BIT;
    gzip::BitReader bitReader( std::make_unique<BufferedFileReader>( std::move( data ) ) );

    uint64_t candidateCount{ 0 };

    using namespace rapidgzip::blockfinder;

    rapidgzip::deflate::Block block;
    for ( size_t offset = 0; offset <= nBitsToTest; ) {
        bitReader.seek( static_cast<long long int>( offset ) );

        try
        {
            const auto peeked = bitReader.peek<CACHED_BIT_COUNT>();
            const auto nextPosition = NEXT_DYNAMIC_DEFLATE_CANDIDATE_LUT<CACHED_BIT_COUNT>[peeked];

            /* If we can skip forward, then that means that the new position only has been partially checked.
             * Therefore, rechecking the LUT for non-zero skips not only ensures that we aren't wasting time in
             * readHeader but it also ensures that we can avoid checking the first three bits again inside readHeader
             * and instead start reading and checking the dynamic Huffman code directly! */
            if ( nextPosition > 0 ) {
                bitReader.seekAfterPeek( nextPosition );
                offset += nextPosition;
                continue;
            }

            ++candidateCount;
            ++offset;
        } catch ( const gzip::BitReader::EndOfFileReached& ) {
            /* This might happen when calling readDynamicHuffmanCoding quite some bytes before the end! */
            break;
        }
    }

    return candidateCount;
}


template<uint8_t CACHED_BIT_COUNT>
[[nodiscard]] uint64_t
countDeflateBlocksPreselectionManualSlidingBuffer( BufferedFileReader::AlignedBuffer data )
{
    const size_t nBitsToTest = data.size() * CHAR_BIT;
    gzip::BitReader bitReader( std::make_unique<BufferedFileReader>( std::move( data ) ) );

    uint64_t candidateCount{ 0 };

    using namespace rapidgzip::blockfinder;

    /* For this test, CACHED_BIT_COUNT (<=18) would be sufficient but for the precode check we would need in total
     * 13 + 4 + 57 = 74 bits. We might split this into two buffers of length CACHED_BIT_COUNT and 74 -CACHED_BIT_COUNT
     * because we need the CACHED_BIT_COUNT anyway for much more frequent LUT lookup. */
    auto bitBufferForLUT = bitReader.read<CACHED_BIT_COUNT>();

    rapidgzip::deflate::Block block;
    try {
        for ( size_t offset = 0; offset <= nBitsToTest; ) {
            auto nextPosition = NEXT_DYNAMIC_DEFLATE_CANDIDATE_LUT<CACHED_BIT_COUNT>[bitBufferForLUT];

            /* If we can skip forward, then that means that the new position only has been partially checked.
             * Therefore, rechecking the LUT for non-zero skips not only ensures that we aren't wasting time in
             * readHeader but it also ensures that we can avoid checking the first three bits again inside readHeader
             * and instead start reading and checking the dynamic Huffman code directly! */
            if ( nextPosition == 0 ) {
                nextPosition = 1;
                ++candidateCount;
            }

            bitBufferForLUT >>= nextPosition;
            bitBufferForLUT |= bitReader.read( nextPosition )
                               << static_cast<uint8_t>( CACHED_BIT_COUNT - nextPosition );
            offset += nextPosition;
        }
    } catch ( const gzip::BitReader::EndOfFileReached& ) {
        /* This might happen when calling readDynamicHuffmanCoding quite some bytes before the end! */
    }

    return candidateCount;
}


enum class CheckPrecodeMethod
{
    WITHOUT_LUT,
    WITHOUT_LUT_USING_ARRAY,
    WALK_TREE_LUT,
    WALK_TREE_COMPRESSED_LUT,  // Deduplicate the very sparse WALK_TREE_LUT by using an indirect LUTs to chunks.
    WALK_TREE_COMPRESSED_SINGLE_LUT,  // WALK_TREE_COMPRESSED_LUT taken to the extreme, i.e., up to code length 7.
    WALK_TREE_COMPRESSED_SINGLE_LUT_2,  // Ibid, but slightly bit-compressed for size.
    SINGLE_LUT,  // same as WALK_TREE_COMPRESSED_SINGLE_LUT but variable-length bits for histogram.
    SINGLE_COMPRESSED_LUT,
    VALID_HISTOGRAMS_LUT,  // Looks up histogram in the LUT of 1576 valid ones and reuses cached precode huffman codings
};


[[nodiscard]] std::string
toString( CheckPrecodeMethod method )
{
    switch ( method )
    {
    case CheckPrecodeMethod::WITHOUT_LUT                      : return "Without LUT";
    case CheckPrecodeMethod::WITHOUT_LUT_USING_ARRAY          : return "Without LUT Using Array";
    case CheckPrecodeMethod::WALK_TREE_LUT                    : return "Walk Tree LUT";
    case CheckPrecodeMethod::WALK_TREE_COMPRESSED_LUT         : return "Walk Tree Compressed LUT";
    case CheckPrecodeMethod::WALK_TREE_COMPRESSED_SINGLE_LUT  : return "Walk Tree Compressed Single LUT";
    case CheckPrecodeMethod::WALK_TREE_COMPRESSED_SINGLE_LUT_2: return "Walk Tree Compressed Single LUT (optimized)";
    case CheckPrecodeMethod::SINGLE_LUT                       : return "Single LUT";
    case CheckPrecodeMethod::SINGLE_COMPRESSED_LUT            : return "Single Compressed LUT";
    case CheckPrecodeMethod::VALID_HISTOGRAMS_LUT             : return "Valid Histograms LUT";
    }
    throw std::invalid_argument( "Unknown check precode method!" );
}


constexpr auto OPTIMAL_CHECK_PRECODE_METHOD = CheckPrecodeMethod::SINGLE_COMPRESSED_LUT;


#if 0
static constexpr uint8_t CLS_TO_CHECK = 6U;

static const std::array<bool, ( 1U << ( CLS_TO_CHECK * rapidgzip::deflate::PRECODE_BITS ) )> PRECHECK_PRECODE_LUT =
    [] () {
        using CodeLengthsHistogram = std::array<uint8_t, 8>;

        const auto checkHistogram =
            [] ( const CodeLengthsHistogram& bitLengthFrequencies )
            {
                /* Note that bitLengthFrequencies[0] must not be checked because multiple symbols may have code length
                 * 0 simply when they do not appear in the text at all! And this may very well happen because the
                 * order for the code lengths per symbol in the bit stream is fixed. */
                bool invalidCodeLength{ false };
                uint32_t unusedSymbolCount{ 2 };
                constexpr auto MAX_LENGTH = ( 1U << rapidgzip::deflate::PRECODE_BITS );
                for ( size_t bitLength = 1; bitLength < MAX_LENGTH; ++bitLength ) {
                    const auto frequency = bitLengthFrequencies[bitLength];
                    invalidCodeLength |= frequency > unusedSymbolCount;
                    unusedSymbolCount -= static_cast<uint32_t>( frequency );
                    unusedSymbolCount *= 2;  /* Because we go down one more level for all unused tree nodes! */
                }
                return !invalidCodeLength;
            };

        std::array<bool, ( 1U << ( CLS_TO_CHECK * rapidgzip::deflate::PRECODE_BITS ) )> result{};

        for ( uint32_t bits = 0; bits < result.size(); ++bits ) {
            CodeLengthsHistogram histogram{};
            for ( size_t i = 0; i < CLS_TO_CHECK; ++i ) {
                const auto cl = ( bits >> ( i * 3U ) ) & 0b111U;
                histogram[cl]++;
            }
            if ( checkHistogram( histogram ) ) {
                result[bits] = true;
            }
        }

        return result;
    } ();
#endif


template<CheckPrecodeMethod CHECK_PRECODE_METHOD>
constexpr rapidgzip::Error
checkPrecode( const uint64_t next4Bits,
              const uint64_t next57Bits )
{
    using namespace rapidgzip::PrecodeCheck;

    if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::SINGLE_COMPRESSED_LUT ) {
        /**
         * I'm completely baffled that there is no performance gain for this one, which requires
         * only 78 KiB 256 B LUT as opposed to SingleLUT, which requires a 2 MiB LUT.
         * The lookup itself also isn't more expensive because the same bits are now stored in bytes,
         * which avoids a third stage of bit-shifting and masking.
         * -> Finally similarly fast, although not faster than WALK_TREE_COMPRESSED_SINGLE_LUT by
         *    avoiding the overflow check in favor of false positives!
         *    On my notebook, it actually is ~10% faster than WALK_TREE_COMPRESSED_SINGLE_LUT!
         * @verbatim
         * [13 bits] ( 65.2 <= 70.9 +- 2.5 <= 73.2 ) MB/s
         * [14 bits] ( 65.4 <= 69.3 +- 2.5 <= 72.7 ) MB/s
         * [15 bits] ( 65.7 <= 69.4 +- 1.8 <= 71.7 ) MB/s
         * [16 bits] ( 69.0 <= 70.7 +- 0.9 <= 71.9 ) MB/s
         * [17 bits] ( 68.0 <= 71.0 +- 1.6 <= 72.6 ) MB/s
         * [18 bits] ( 60   <= 69   +- 3   <= 72   ) MB/s
         * @endverbatim
         */
        return SingleCompressedLUT::checkPrecode( next4Bits, next57Bits );
    }

    if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::SINGLE_LUT ) {
        /**
         * I thought this would be faster than the WalkTreeLUT, it even save the whole branch
         * in case the precode might be valid judging from the first 5 frequency counts.
         * But the overflow checking might add too much more instructions in all cases.
         * @verbatim
         * [13 bits] ( 66.2 <= 70.0 +- 1.4 <= 71.6 ) MB/s
         * [14 bits] ( 54   <= 70   +- 6   <= 74   ) MB/s
         * [15 bits] ( 62.7 <= 70.1 +- 2.7 <= 71.7 ) MB/s
         * [16 bits] ( 68.1 <= 70.6 +- 1.0 <= 71.5 ) MB/s
         * [17 bits] ( 69.5 <= 70.0 +- 0.5 <= 71.0 ) MB/s
         * [18 bits] ( 58   <= 66   +- 3   <= 70   ) MB/s
         * @endverbatim
         */
        return SingleLUT::checkPrecode( next4Bits, next57Bits );
    }

    if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_COMPRESSED_LUT ) {
        /**
         * @verbatim
         * [13 bits] ( 65.2 <= 66.3 +- 0.4 <= 66.7 ) MB/s
         * [14 bits] ( 60.3 <= 64.7 +- 1.8 <= 65.8 ) MB/s
         * [15 bits] ( 61.9 <= 63.4 +- 1.6 <= 65.6 ) MB/s
         * [16 bits] ( 62.6 <= 63.4 +- 0.5 <= 64.3 ) MB/s
         * [17 bits] ( 63.6 <= 65.3 +- 1.0 <= 66.7 ) MB/s
         * [18 bits] ( 63.3 <= 64.9 +- 0.8 <= 65.9 ) MB/s
         * @endverbatim
         */
        /* Size-optimal parameter combinations: (5, 16), (6, 64), (7, 512) */
        constexpr auto PRECODE_FREQUENCIES_LUT_COUNT = 5U;
        constexpr auto SUBTABLE_CHUNK_COUNT = 16U;
        return WalkTreeCompressedLUT::checkPrecode<
            PRECODE_FREQUENCIES_LUT_COUNT, SUBTABLE_CHUNK_COUNT>( next4Bits, next57Bits );
    }

    if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_COMPRESSED_SINGLE_LUT ) {
        /**
         * @verbatim
         * [13 bits] ( 69.1 <= 70.9 +- 1.4 <= 73.1 ) MB/s
         * [14 bits] ( 66.0 <= 68.3 +- 1.1 <= 70.1 ) MB/s
         * [15 bits] ( 68.9 <= 70.0 +- 0.9 <= 71.9 ) MB/s
         * [16 bits] ( 70.7 <= 72.5 +- 0.8 <= 73.4 ) MB/s
         * [17 bits] ( 69.4 <= 72.0 +- 1.7 <= 73.7 ) MB/s
         * [18 bits] ( 67.7 <= 70.3 +- 1.3 <= 71.3 ) MB/s
         * @endverbatim
         */
        /* Size-optimal parameter combinations: (5, 16), (6, 64), (7, 512) */
        constexpr auto PRECODE_FREQUENCIES_LUT_COUNT = 7U;
        constexpr auto SUBTABLE_CHUNK_COUNT = 512U;
        return WalkTreeCompressedLUT::checkPrecode<
            PRECODE_FREQUENCIES_LUT_COUNT, SUBTABLE_CHUNK_COUNT>( next4Bits, next57Bits );
    }

    if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_COMPRESSED_SINGLE_LUT_2 ) {
        /**
         * @verbatim
         * [13 bits, chunk count 128] ( 71.4 <= 73.5 +- 1.0 <= 74.5 ) MB/s
         * [14 bits, chunk count 128] ( 72.5 <= 73.0 +- 0.5 <= 73.6 ) MB/s
         * [15 bits, chunk count 128] ( 68.9 <= 71.8 +- 1.1 <= 72.6 ) MB/s
         * [16 bits, chunk count 128] ( 69.8 <= 70.9 +- 1.1 <= 73.1 ) MB/s
         * [17 bits, chunk count 128] ( 66.7 <= 71.0 +- 2.2 <= 74.3 ) MB/s
         * [18 bits, chunk count 128] ( 68.6 <= 71.4 +- 1.0 <= 72.0 ) MB/s
         * @endverbatim
         */
        return WalkTreeCompressedSingleLUT::checkPrecode( next4Bits, next57Bits );
    }

    if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_LUT ) {
        /**
         * Even this version with 40 KiB is not faster than the version with 4 MiB.
         * It's actually a tad slower, especially for the minimum measured bandwidths.
         * I'm baffled.
         * @todo Compressing the LUT might have an actual benefit when including one more count.
         *       The uncompressed LUT for that is 128 MiB! But, theoretically, the upper bound for
         *       the compressed LUT would be 32 * 40 KiB = 1280 KiB but I need to fix the creation
         *       algorithm to skip a temporary creation of the 128 MiB table, especially if I want
         *       to have it constexpr.
         * @verbatim
         * [13 bits] ( 65.3 <= 66.3 +- 0.5 <= 66.7 ) MB/s
         * [14 bits] ( 62.3 <= 63.8 +- 1.6 <= 66.6 ) MB/s
         * [15 bits] ( 62.3 <= 64.6 +- 1.1 <= 66.1 ) MB/s
         * [16 bits] ( 62.5 <= 64.8 +- 1.4 <= 66.0 ) MB/s
         * [17 bits] ( 64.3 <= 65.7 +- 0.7 <= 66.5 ) MB/s
         * [18 bits] ( 63.3 <= 64.1 +- 0.3 <= 64.4 ) MB/s
         * @endverbatim
         */
        return WalkTreeLUT::checkPrecode( next4Bits, next57Bits );
    }

    if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WITHOUT_LUT_USING_ARRAY ) {
        /**
         * @verbatim
         * [13 bits] ( 38.3  <= 39.1  +- 0.8  <= 40.6  ) MB/s
         * [14 bits] ( 40.63 <= 41.05 +- 0.22 <= 41.23 ) MB/s
         * [15 bits] ( 39.3  <= 39.9  +- 0.5  <= 41.1  ) MB/s
         * [16 bits] ( 35.4  <= 38.7  +- 1.2  <= 39.5  ) MB/s
         * [17 bits] ( 37.8  <= 39.0  +- 0.9  <= 40.0  ) MB/s
         * [18 bits] ( 37.9  <= 38.3  +- 0.6  <= 39.8  ) MB/s
         * @endverbatim
         */
        return WithoutLUT::checkPrecodeUsingArray( next4Bits, next57Bits );
    }

    if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WITHOUT_LUT ) {
        /**
         * @verbatim
         * [13 bits] ( 36.3 <= 37.2 +- 0.5 <= 37.9 ) MB/s
         * [14 bits] ( 37.9 <= 38.5 +- 0.3 <= 38.8 ) MB/s
         * [15 bits] ( 37.9 <= 38.7 +- 0.5 <= 39.1 ) MB/s
         * [16 bits] ( 36.6 <= 37.8 +- 0.6 <= 38.6 ) MB/s
         * [17 bits] ( 34.0 <= 34.7 +- 0.6 <= 36.1 ) MB/s
         * [18 bits] ( 35.5 <= 37.4 +- 0.9 <= 38.3 ) MB/s
         * @endverbatim
         */
        return WithoutLUT::checkPrecode( next4Bits, next57Bits );
    }

    throw std::invalid_argument( "Unknown check precode method!" );
}


[[nodiscard]] std::optional<std::pair<size_t, uint64_t> >
checkAndGetValidHistogramID( const uint64_t precodeBits )
{
    using namespace rapidgzip;
    using namespace deflate;
    using namespace PrecodeCheck::SingleCompressedLUT;
    using rapidgzip::PrecodeCheck::SingleLUT::Histogram;

    constexpr auto PRECODES_PER_CHUNK = 4U;
    constexpr auto CACHED_BITS = PRECODE_BITS * PRECODES_PER_CHUNK;
    constexpr auto CHUNK_COUNT = ceilDiv( rapidgzip::deflate::MAX_PRECODE_COUNT, PRECODES_PER_CHUNK );
    static_assert( CACHED_BITS == 12 );
    static_assert( CHUNK_COUNT == 5 );

    Histogram bitLengthFrequencies{ 0 };
    Histogram overflowsInSum{ 0 };
    Histogram overflowsInLUT{ 0 };

    for ( size_t chunk = 0; chunk < CHUNK_COUNT; ++chunk ) {
        auto precodeChunk = precodeBits >> ( chunk * CACHED_BITS );
        /* The last requires no bit masking because @ref next57Bits is already sufficiently masked.
         * This branch will hopefully get unrolled, else it could hinder performance. */
        if ( chunk != CHUNK_COUNT - 1 ) {
            precodeChunk &= nLowestBitsSet<uint64_t, CACHED_BITS>();
        }

        const auto partialHistogram = PRECODE_X4_TO_HISTOGRAM_LUT[precodeChunk];

        /**
         * Account for overflows over the storage boundaries during addition.
         *  - Addition in lowest bits: 0+0 -> 0, 0+1 -> 1, 1+0 -> 1, 1+1 -> 0 (+ carry bit)
         *                             <=> bitwise xor ^ (also sometimes called carryless addition)
         *  - If there is a carry-over (overflow) from a lower bit, then these results will be inverted.
         *    We can check for that with another xor, which also acts as a bit-wise inequality comparison,
         *    setting the resulting bit only to 1 if both source bits are different.
         *    This result needs to be masked to the bits of interest but that can be done last to reduce instructions.
         */
        const auto carrylessSum = bitLengthFrequencies ^ partialHistogram;
        bitLengthFrequencies = bitLengthFrequencies + partialHistogram;
        overflowsInSum |= carrylessSum ^ bitLengthFrequencies;
        overflowsInLUT |= partialHistogram;
    }

    /* Ignore non-zero and overflow counts for lookup. */
    const auto histogramToLookUp = ( bitLengthFrequencies >> 5U )
                                   & nLowestBitsSet<Histogram>( HISTOGRAM_TO_LOOK_UP_BITS );
    const auto nonZeroCount = bitLengthFrequencies & nLowestBitsSet<Histogram>( 5 );
    if ( LIKELY( POWER_OF_TWO_SPECIAL_CASES[nonZeroCount] != histogramToLookUp ) ) [[likely]] {
        if ( ( ( overflowsInSum & OVERFLOW_BITS_MASK ) != 0 )
             || ( ( overflowsInLUT & ( ~Histogram( 0 ) << OVERFLOW_MEMBER_OFFSET ) ) != 0 ) ) {
            return std::nullopt;
        }

        constexpr auto COMPRESSED_LUT_CHUNK_COUNT = 8U;
        const auto& [histogramLUT, validLUT] =
            PrecodeCheck::SingleCompressedLUT::COMPRESSED_PRECODE_HISTOGRAM_VALID_LUT_DICT<COMPRESSED_LUT_CHUNK_COUNT>;
        constexpr auto INDEX_BITS = requiredBits( COMPRESSED_LUT_CHUNK_COUNT * sizeof( uint64_t ) * CHAR_BIT );
        const auto elementIndex = ( histogramToLookUp >> INDEX_BITS )
                                  & nLowestBitsSet<Histogram>( HISTOGRAM_TO_LOOK_UP_BITS - INDEX_BITS );
        const auto subIndex = histogramLUT[elementIndex];
        const auto validIndex = ( subIndex << INDEX_BITS )
                                + ( histogramToLookUp & nLowestBitsSet<uint64_t>( INDEX_BITS ) );
        if ( LIKELY( ( validLUT[validIndex] ) == 0 ) ) [[unlikely]] {
            /* This also handles the case of all being zero, which in the other version returns EMPTY_ALPHABET!
             * Some might also not be bloating but simply invalid, we cannot differentiate that but it can be
             * helpful for tests to have different errors. For actual usage comparison with NONE is sufficient. */
            return std::nullopt;
        }
    }

    using PrecodeCheck::SingleLUT::ValidHistogramID::getHistogramIdFromVLPHWithoutZero;
    const auto validId = getHistogramIdFromVLPHWithoutZero( histogramToLookUp );
    return std::make_pair( validId, histogramToLookUp );
}


/* Without "forceinline", I observed a ~10% performance degradation! */
template<CheckPrecodeMethod CHECK_PRECODE_METHOD>
[[nodiscard]] forceinline rapidgzip::Error
checkDeflateBlock( const uint64_t   bitBufferForLUT,
                   const uint64_t   bitBufferPrecodeBits,
                   const size_t     offset,
                   gzip::BitReader& bitReader )
{
    using namespace rapidgzip;
    using namespace deflate;
    constexpr auto ALL_PRECODE_BITS = PRECODE_COUNT_BITS + MAX_PRECODE_COUNT * PRECODE_BITS;

    const auto next4Bits = bitBufferPrecodeBits & nLowestBitsSet<uint64_t, PRECODE_COUNT_BITS>();
    const auto next57Bits = ( bitBufferPrecodeBits >> PRECODE_COUNT_BITS )
                            & nLowestBitsSet<uint64_t, MAX_PRECODE_COUNT * PRECODE_BITS>();

    auto error = Error::NONE;
    [[maybe_unused]] std::optional<std::pair<size_t, uint64_t> > lookedUpHistogramID;
    if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::VALID_HISTOGRAMS_LUT ) {
        /**
         * @verbatim
         * [13 bits] ( 51.3 <= 52.8 +- 0.9 <= 54.4 ) MB/s
         * [14 bits] ( 45.1 <= 52.4 +- 2.9 <= 54.4 ) MB/s
         * [15 bits] ( 46.7 <= 51.6 +- 2.0 <= 54.0 ) MB/s
         * [16 bits] ( 51.2 <= 53.7 +- 1.1 <= 54.5 ) MB/s
         * [17 bits] ( 50.5 <= 55.3 +- 2.0 <= 57.3 ) MB/s
         * [18 bits] ( 53.0 <= 54.6 +- 0.8 <= 55.3 ) MB/s
         * @endverbatim
         */
        const auto codeLengthCount = 4 + next4Bits;
        const auto precodeBits = next57Bits & nLowestBitsSet<uint64_t>( codeLengthCount * PRECODE_BITS );
        /* This would be inside a specialized checkPrecode, however, this check also gives us the ID (<=1576),
         * which can be used to load and use a compile-time initialized PrecodeHuffmanCoding object from a LUT! */
        lookedUpHistogramID = checkAndGetValidHistogramID( precodeBits );
        if ( !lookedUpHistogramID || ( lookedUpHistogramID->first >= precode::VALID_HUFFMAN_CODINGS.size() ) ) {
            return Error::INVALID_CODE_LENGTHS;
        }
    } else {
        error = checkPrecode<CHECK_PRECODE_METHOD>( next4Bits, next57Bits );
    }

    if ( LIKELY( error != Error::NONE ) ) [[unlikely]] {
        return error;
    }

#ifndef NDEBUG
    const auto oldTell = bitReader.tell();
#endif

    const auto literalCodeCount = 257 + ( ( bitBufferForLUT >> 3U ) & nLowestBitsSet<uint64_t, 5>() );
    const auto distanceCodeCount = 1 + ( ( bitBufferForLUT >> 8U ) & nLowestBitsSet<uint64_t, 5>() );
    const auto codeLengthCount = 4 + next4Bits;
    const auto precodeBits = next57Bits & nLowestBitsSet<uint64_t>( codeLengthCount * PRECODE_BITS );

    LiteralAndDistanceCLBuffer literalCL{};
    const auto distanceCodesOffset = offset + 13 + 4 + ( codeLengthCount * PRECODE_BITS );
    const auto bitReaderOffset = offset + 13 + ALL_PRECODE_BITS;

    if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::VALID_HISTOGRAMS_LUT ) {
        const auto& precodeHC = precode::VALID_HUFFMAN_CODINGS[lookedUpHistogramID->first];

        //using rapidgzip::PrecodeCheck::SingleLUT::getAlphabetFromCodeLengths;
        /* I would need a *another* POWER_OF_TWO_SPECIAL_CASES LUT to get alphabets for those cases :/ */
        //const auto alphabet = getAlphabetFromCodeLengths( precodeBits, bitLengthFrequencies );
        const auto histogram = PrecodeCheck::WalkTreeLUT::precodesToHistogram<5>( precodeBits );
        const auto alphabet = precode::getAlphabetFromCodeLengths( precodeBits, histogram );

        bitReader.seek( static_cast<long long int>( distanceCodesOffset ) );

        error = readDistanceAndLiteralCodeLengths(
            literalCL, bitReader, precodeHC, literalCodeCount + distanceCodeCount,
            [&alphabet] ( auto symbol ) { return alphabet[symbol]; } );
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    } else if constexpr ( false /* CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_LUT */ ) {
        /** @todo This fails for the igzip benchmark! */
        using PrecodeCheck::SingleLUT::ValidHistogramID::getHistogramIdFromUniformlyPackedHistogram;
        const auto histogram = PrecodeCheck::WalkTreeLUT::precodesToHistogram<5>( precodeBits );
        const auto validId = getHistogramIdFromUniformlyPackedHistogram( histogram );
        if ( validId >= precode::VALID_HUFFMAN_CODINGS.size() ) {
            throw std::logic_error( "Histograms should have been valid at this point!" );
        }
        const auto& precodeHC = precode::VALID_HUFFMAN_CODINGS[validId];

        bitReader.seek( static_cast<long long int>( distanceCodesOffset ) );
        const auto alphabet = precode::getAlphabetFromCodeLengths( precodeBits, histogram );
        error = readDistanceAndLiteralCodeLengths(
            literalCL, bitReader, precodeHC, literalCodeCount + distanceCodeCount,
            [&alphabet] ( auto symbol ) { return alphabet[symbol]; } );
    } else {
        /* Get code lengths (CL) for alphabet P. */
        std::array<uint8_t, MAX_PRECODE_COUNT> codeLengthCL{};
        for ( size_t i = 0; i < codeLengthCount; ++i ) {
            const auto codeLength = ( precodeBits >> ( i * PRECODE_BITS ) )
                                    & nLowestBitsSet<uint64_t, PRECODE_BITS>();
            codeLengthCL[PRECODE_ALPHABET[i]] = codeLength;
        }

        PrecodeHuffmanCoding precodeHC;
        error = precodeHC.initializeFromLengths( { codeLengthCL.data(), codeLengthCL.size() } );

        /* Note that the precode should never fail to initialize because checkPrecode
         * already returned successful! */
        if ( UNLIKELY( error != Error::NONE ) ) [[unlikely]] {
            return error;
        }

        bitReader.seek( static_cast<long long int>( distanceCodesOffset ) );
        error = readDistanceAndLiteralCodeLengths(
            literalCL, bitReader, precodeHC, literalCodeCount + distanceCodeCount );
    }

    /* Using this theoretically derivable position avoids a possibly costly call to tell()
     * to save the old offset. */
    bitReader.seek( static_cast<long long int>( bitReaderOffset ) );

    if ( LIKELY( error != Error::NONE ) ) [[likely]] {
        return error;
    }

    if ( literalCL[deflate::END_OF_BLOCK_SYMBOL] == 0 ) {
        return Error::INVALID_CODE_LENGTHS;
    }

    /* Check distance code lengths. */
    HuffmanCodingCheckOnly<uint16_t, MAX_CODE_LENGTH,
                           uint8_t, MAX_DISTANCE_SYMBOL_COUNT> distanceHC;
    error = distanceHC.initializeFromLengths(
        VectorView<uint8_t>( literalCL.data() + literalCodeCount, distanceCodeCount ) );

    if ( LIKELY( error != Error::NONE ) ) [[likely]] {
        return error;
    }

    /* Check literal code lengths. */
    HuffmanCodingCheckOnly<uint16_t, MAX_CODE_LENGTH,
                           uint16_t, MAX_LITERAL_HUFFMAN_CODE_COUNT> literalHC;
    error = literalHC.initializeFromLengths( VectorView<uint8_t>( literalCL.data(), literalCodeCount ) );

#ifndef NDEBUG
    if ( oldTell != bitReader.tell() ) {
        std::cerr << "Previous position: " << oldTell << " new position: " << bitReader.tell() << "\n";
        throw std::logic_error( "Did not seek back correctly!" );
    }
#endif

    return error;
}


/**
 * Same as findDeflateBlocksRapidgzip but prefilters calling rapidgzip using a lookup table and even skips multiple bits.
 * Also, does not find uncompressed blocks nor fixed huffman blocks and as the others no final blocks!
 * The idea is that fixed huffman blocks should be very rare and uncompressed blocks can be found very fast in a
 * separate run over the data (to be implemented).
 */
template<uint8_t            CACHED_BIT_COUNT,
         CheckPrecodeMethod CHECK_PRECODE_METHOD = OPTIMAL_CHECK_PRECODE_METHOD>
[[nodiscard]] std::vector<size_t>
findDeflateBlocksRapidgzipLUT( BufferedFileReader::AlignedBuffer data )
{
    const size_t nBitsToTest = data.size() * CHAR_BIT;
    gzip::BitReader bitReader( std::make_unique<BufferedFileReader>( std::move( data ) ) );

    std::vector<size_t> bitOffsets;

    const auto oldOffset = bitReader.tell();

    try
    {
        using namespace rapidgzip;
        using namespace rapidgzip::deflate;  /* For the definitions of deflate-specific number of bits. */

        /**
         * For LUT we need at CACHED_BIT_COUNT bits and for the precode check we would need in total
         * 13 + 4 + 57 = 74 bits. Because this does not fit into 64-bit we need to keep two sliding bit buffers.
         * The first can simply have length CACHED_BIT_COUNT and the other one can even keep duplicated bits to
         * have length of 61 bits required for the precode. Updating three different buffers would require more
         * instructions but might not be worth it.
         */
        auto bitBufferForLUT = bitReader.peek<CACHED_BIT_COUNT>();
        bitReader.seek( static_cast<long long int>( oldOffset ) + 13 );
        constexpr auto ALL_PRECODE_BITS = PRECODE_COUNT_BITS + MAX_PRECODE_COUNT * PRECODE_BITS;
        static_assert( ( ALL_PRECODE_BITS == 61 ) && ( ALL_PRECODE_BITS >= CACHED_BIT_COUNT )
                       && ( ALL_PRECODE_BITS <= std::numeric_limits<uint64_t>::digits )
                       && ( ALL_PRECODE_BITS <= gzip::BitReader::MAX_BIT_BUFFER_SIZE ),
                       "It must fit into 64-bit and it also must fit the largest possible jump in the LUT." );
        auto bitBufferPrecodeBits = bitReader.read<ALL_PRECODE_BITS>();

        const auto& LUT = rapidgzip::blockfinder::NEXT_DYNAMIC_DEFLATE_CANDIDATE_LUT<CACHED_BIT_COUNT>;
        Block block;
        for ( size_t offset = oldOffset; offset <= nBitsToTest; ) {
            auto nextPosition = LUT[bitBufferForLUT];  // ~8 MB/s

            /* If we can skip forward, then that means that the new position only has been partially checked.
             * Therefore, rechecking the LUT for non-zero skips not only ensures that we aren't wasting time in
             * readHeader but it also ensures that we can avoid checking the first three bits again inside readHeader
             * and instead start reading and checking the dynamic Huffman code directly! */
            if ( nextPosition == 0 ) {
                nextPosition = 1;

                const auto error = checkDeflateBlock<CHECK_PRECODE_METHOD>( bitBufferForLUT, bitBufferPrecodeBits,
                                                                            offset, bitReader );
                if ( UNLIKELY( error == Error::NONE ) ) [[unlikely]] {
                    /* Testing decoding is not necessary because the Huffman canonical check is already very strong!
                     * Decoding up to 8 KiB like in pugz only impedes performance and it is harder to reuse that
                     * already decoded data if we do decide that it is a valid block. The number of checks during
                     * reading is also pretty few because there almost are no wasted / invalid symbols. */
                    bitOffsets.push_back( offset );
                }
            }

            const auto bitsToLoad = nextPosition;

            /* Refill bit buffer for LUT using the bits from the higher precode bit buffer. */
            bitBufferForLUT >>= bitsToLoad;
            if constexpr ( CACHED_BIT_COUNT > 13 ) {
                constexpr uint8_t DUPLICATED_BITS = CACHED_BIT_COUNT - 13;
                bitBufferForLUT |= ( ( bitBufferPrecodeBits >> DUPLICATED_BITS )
                                     & nLowestBitsSet<uint64_t>( bitsToLoad ) )
                                   << static_cast<uint8_t>( CACHED_BIT_COUNT - bitsToLoad );
            } else {
                bitBufferForLUT |= ( bitBufferPrecodeBits & nLowestBitsSet<uint64_t>( bitsToLoad ) )
                                   << static_cast<uint8_t>( CACHED_BIT_COUNT - bitsToLoad );
            }

            /* Refill the precode bit buffer directly from the bit reader. */
            bitBufferPrecodeBits >>= bitsToLoad;
            bitBufferPrecodeBits |= bitReader.read( bitsToLoad )
                                    << static_cast<uint8_t>( ALL_PRECODE_BITS - bitsToLoad );

            offset += nextPosition;
        }
    } catch ( const gzip::BitReader::EndOfFileReached& ) {
        /* This might happen when calling readDynamicHuffmanCoding quite some bytes before the end! */
    }

    return bitOffsets;
}


[[nodiscard]] std::vector<size_t>
countFilterEfficiencies( BufferedFileReader::AlignedBuffer data )
{
    const size_t nBitsToTest = data.size() * CHAR_BIT;
    gzip::BitReader bitReader( std::make_unique<BufferedFileReader>( std::move( data ) ) );

    std::vector<size_t> bitOffsets;

    using namespace rapidgzip::blockfinder;
    static constexpr auto CACHED_BIT_COUNT = 14;

    size_t offsetsTestedMoreInDepth{ 0 };
    std::unordered_map<rapidgzip::Error, uint64_t> errorCounts;
    rapidgzip::deflate::Block</* enable analysis */ true> block;
    size_t checkPrecodeFails{ 0 };
    size_t passedDeflateHeaderTest{ 0 };
    for ( size_t offset = 0; offset <= nBitsToTest; ) {
        bitReader.seek( static_cast<long long int>( offset ) );

        try
        {
            const auto peeked = bitReader.peek<CACHED_BIT_COUNT>();
            const auto nextPosition = NEXT_DYNAMIC_DEFLATE_CANDIDATE_LUT<CACHED_BIT_COUNT>[peeked];

            if ( nextPosition > 0 ) {
                bitReader.seekAfterPeek( nextPosition );
                offset += nextPosition;
                continue;
            }
            ++passedDeflateHeaderTest;

            bitReader.seek( static_cast<long long int>( offset ) + 13 );
            const auto next4Bits = bitReader.read( rapidgzip::deflate::PRECODE_COUNT_BITS );
            const auto next57Bits = bitReader.peek( rapidgzip::deflate::MAX_PRECODE_COUNT
                                                    * rapidgzip::deflate::PRECODE_BITS );
            static_assert( rapidgzip::deflate::MAX_PRECODE_COUNT * rapidgzip::deflate::PRECODE_BITS
                           <= gzip::BitReader::MAX_BIT_BUFFER_SIZE,
                           "This optimization requires a larger BitBuffer inside BitReader!" );
            using rapidgzip::PrecodeCheck::WalkTreeLUT::checkPrecode;
            const auto precodeError = checkPrecode( next4Bits, next57Bits );
            if ( precodeError != rapidgzip::Error::NONE ) {
                ++checkPrecodeFails;
            }

            offsetsTestedMoreInDepth++;
            bitReader.seek( static_cast<long long int>( offset ) + 3 );
            auto error = precodeError;
            if ( precodeError == rapidgzip::Error::NONE ) {
                error = block.readDynamicHuffmanCoding( bitReader );
            }

            const auto [count, wasInserted] = errorCounts.try_emplace( error, 1 );
            if ( !wasInserted ) {
                count->second++;
            }

            if ( error != rapidgzip::Error::NONE ) {
                ++offset;
                continue;
            }

            bitOffsets.push_back( offset );
            ++offset;
        } catch ( const gzip::BitReader::EndOfFileReached& ) {
            /* This might happen when calling readDynamicHuffmanCoding quite some bytes before the end! */
            break;
        }
    }

    /* From 101984512 bits to test, found 10793213 (10.5832 %) candidates and reduced them down further to 494. */
    std::cerr << "From " << nBitsToTest << " bits to test, found " << offsetsTestedMoreInDepth << " ("
              << static_cast<double>( offsetsTestedMoreInDepth ) / static_cast<double>( nBitsToTest ) * 100
              << " %) candidates and reduced them down further to " << bitOffsets.size() << ".\n";

    /**
     * @verbatim
     * Invalid Precode  HC: 10750093
     * Invalid Distance HC: 8171
     * Invalid Symbol   HC: 76
     * @endverbatim
     * This signifies a LOT of optimization potential! We might be able to handle precode checks faster!
     * Note that the maximum size of the precode coding can only be 3*19 bits = 57 bits!
     *  -> Note that BitReader::peek should be able to peek all of these on a 64-bit system even when only able to
     *     append full bytes to the 64-bit buffer because 64-57=7! I.e., 57 is the first case for which it wouldn't
     *     be able to further add to the bit buffer but anything smaller and it is able to insert a full byte!
     *     Using peek can avoid costly buffer-refilling seeks back!
     *     -> Unfortunately, we also have to seek back the 17 bits for the deflate block header and the three
     *        code lengths. So yeah, using peek probably will do nothing.
     */
    std::cerr << "Reading dynamic Huffman Code (HC) deflate block failed because the code lengths were invalid:\n"
              << "    Total number of test locations (including those skipped with the jump LUT): " << nBitsToTest
              << "\n"
              << "    Invalid Precode  HC: " << block.failedPrecodeInit  << " ("
              << static_cast<double>( block.failedPrecodeInit ) / static_cast<double>( nBitsToTest ) * 100 << " %)\n"
              << "    Invalid Distance HC: " << block.failedDistanceInit << " ("
              << static_cast<double>( block.failedDistanceInit ) / static_cast<double>( nBitsToTest ) * 100 << " %)\n"
              << "    Invalid Symbol   HC: " << block.failedLiteralInit   << " ("
              << static_cast<double>( block.failedLiteralInit ) / static_cast<double>( nBitsToTest ) * 100 << " %)\n"
              << "    No end-of-block symbol: " << block.missingEOBSymbol   << " ("
              << static_cast<double>( block.missingEOBSymbol ) / static_cast<double>( nBitsToTest ) * 100 << " %)\n"
              << "    Failed checkPrecode calls: " << checkPrecodeFails << "\n\n";

    std::cerr << "Cumulative time spent during tests with deflate::block::readDynamicHuffmanCoding:\n"
              << "    readDynamicHuffmanCoding : " << block.durations.readDynamicHeader << " s\n"
              << "    Read precode             : " << block.durations.readPrecode << " s\n"
              << "    Create precode HC        : " << block.durations.createPrecodeHC << " s\n"
              << "    Apply precode HC         : " << block.durations.applyPrecodeHC << " s\n"
              << "    Create distance HC       : " << block.durations.createDistanceHC << " s\n"
              << "    Create literal HC        : " << block.durations.createLiteralHC << " s\n"
              << "\n";

    std::cerr << "Filtering cascade:\n"
              << "+-> Total number of test locations: " << nBitsToTest
              << "\n"
              << "    Filtered by deflate header test jump LUT: " << ( nBitsToTest - passedDeflateHeaderTest ) << " ("
              << static_cast<double>( nBitsToTest - passedDeflateHeaderTest ) / static_cast<double>( nBitsToTest ) * 100
              << " %)\n"
              << "    Remaining locations to test: " << passedDeflateHeaderTest << "\n"
              << "    +-> Failed checkPrecode calls: " << checkPrecodeFails << " ("
              << static_cast<double>( checkPrecodeFails ) / static_cast<double>( passedDeflateHeaderTest ) * 100
              << " %)\n"
              << "        Remaining locations to test: " << ( passedDeflateHeaderTest - checkPrecodeFails ) << "\n"
              << "        +-> Missing end-of-block symbol: " << block.missingEOBSymbol << " ("
              << static_cast<double>( block.missingEOBSymbol )
                 / static_cast<double>( passedDeflateHeaderTest - checkPrecodeFails ) * 100 << " %)\n"
              << "        +-> Invalid Distance Huffman Coding: " << block.failedDistanceInit << " ("
              << static_cast<double>( block.failedDistanceInit )
                 / static_cast<double>( passedDeflateHeaderTest - checkPrecodeFails ) * 100 << " %)\n"
              << "            Remaining locations: "
              << ( passedDeflateHeaderTest - checkPrecodeFails - block.failedDistanceInit ) << "\n"
              << "            +-> Failing precode HC usage or literal/distance HC construction: "
              << ( passedDeflateHeaderTest - checkPrecodeFails - block.failedDistanceInit - bitOffsets.size() ) << "\n"
              << "                Location candidates: " << bitOffsets.size() << "\n\n";

    /**
     * @verbatim
     *  4 : 657613
     *  5 : 658794
     *  6 : 655429
     *  7 : 667649
     *  8 : 656510
     *  9 : 656661
     * 10 : 649638
     * 11 : 705194
     * 12 : 663376
     * 13 : 662213
     * 14 : 659557
     * 15 : 678194
     * 16 : 670387
     * 17 : 681204
     * 18 : 699319
     * 19 : 771475
     * @endverbatim
     * Because well compressed data is quasirandom, the distribution of the precode code lengths is also pretty even.
     * It is weird, that exactly the longest case appears much more often than the others, same for 7. This means
     * that runs of 1s seem to be more frequent than other things.
     * Unfortunately, this means that a catch-all LUT does not seem feasible.
     */
    std::cerr << "Precode CL count:\n";
    for ( size_t i = 0; i < block.precodeCLHistogram.size(); ++i ) {
        std::cerr << "    " << std::setw( 2 ) << 4 + i << " : " << block.precodeCLHistogram[i] << "\n";
    }
    std::cerr << "\n";

    /**
     * Encountered errors:
     * @verbatim
     * 7114740 Constructing a Huffman coding from the given code length sequence failed!
     * 3643601 The Huffman coding is not optimal!
     *   28976 Invalid number of literal/length codes!
     *    5403 Cannot copy last length because this is the first one!
     *     494 No error.
     * @endverbatim
     * -> 7M downright invalid Huffman codes but *also* ~4M non-optimal Huffman codes.
     *    The latter is kind of a strong criterium that I'm not even sure that all gzip encoders follow!
     */
    std::multimap<uint64_t, rapidgzip::Error, std::greater<> > sortedErrorTypes;
    for ( const auto [error, count] : errorCounts ) {
        sortedErrorTypes.emplace( count, error );
    }
    std::cerr << "Encountered errors:\n";
    for ( const auto& [count, error] : sortedErrorTypes ) {
        std::cerr << "    " << std::setw( 8 ) << count << " " << toString( error ) << "\n";
    }
    std::cerr << "\n";

    return bitOffsets;
}


/**
 * Same as findDeflateBlocksRapidgzipLUT but tries to improve pipelining by going over the data twice.
 * Once, doing simple Boyer-Moore-like string search tests and skips forward and the second time doing
 * extensive tests by loading and checking the dynamic Huffman trees, which might require seeking back.
 */
template<uint8_t            CACHED_BIT_COUNT,
         CheckPrecodeMethod CHECK_PRECODE_METHOD = OPTIMAL_CHECK_PRECODE_METHOD>
[[nodiscard]] std::vector<size_t>
findDeflateBlocksRapidgzipLUTTwoPass( BufferedFileReader::AlignedBuffer data )
{
    static_assert( CACHED_BIT_COUNT >= 13,
                   "The LUT must check at least 13-bits, i.e., up to including the distance "
                   "code length check, to avoid duplicate checks in the precode check!" );

    const size_t nBitsToTest = data.size() * CHAR_BIT;
    gzip::BitReader bitReader( std::make_unique<BufferedFileReader>( std::move( data ) ) );

    std::vector<size_t> bitOffsetCandidates;

    using namespace rapidgzip::blockfinder;
    const auto& LUT = NEXT_DYNAMIC_DEFLATE_CANDIDATE_LUT<CACHED_BIT_COUNT>;

    //const auto t0 = now();
    for ( size_t offset = 0; offset <= nBitsToTest; ) {
        try {
            const auto nextPosition = LUT[bitReader.peek<CACHED_BIT_COUNT>()];
            if ( nextPosition == 0 ) {
                bitOffsetCandidates.push_back( offset );
                ++offset;
                bitReader.seekAfterPeek( 1 );
            } else {
                offset += nextPosition;
                bitReader.seekAfterPeek( nextPosition );
            }
        } catch ( const gzip::BitReader::EndOfFileReached& ) {
            break;
        }
    }

    //const auto t1 = now();
    //std::cerr << "    Candidates after first pass: " << bitOffsetCandidates.size()
    //          << ", pass took " << duration( t0, t1 ) << " s\n";

    std::vector<size_t> bitOffsets;

    rapidgzip::deflate::Block block;

    const auto checkOffset =
        [&] ( const auto offset )
        {
            /* Check the precode Huffman coding. We can skip a lot of the generic tests done in deflate::Block
             * because this is only called for offsets prefiltered by the LUT. But, this also means that the
             * LUT size must be at least 13-bit! */
            try {
                bitReader.seek( static_cast<long long int>( offset ) + 13 );
                const auto next4Bits = bitReader.read( rapidgzip::deflate::PRECODE_COUNT_BITS );
                const auto next57Bits = bitReader.peek( rapidgzip::deflate::MAX_PRECODE_COUNT
                                                        * rapidgzip::deflate::PRECODE_BITS );
                static_assert( rapidgzip::deflate::MAX_PRECODE_COUNT * rapidgzip::deflate::PRECODE_BITS
                               <= gzip::BitReader::MAX_BIT_BUFFER_SIZE,
                               "This optimization requires a larger BitBuffer inside BitReader!" );

                using namespace rapidgzip::PrecodeCheck;
                auto error = rapidgzip::Error::NONE;
                if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::SINGLE_COMPRESSED_LUT ) {
                    error = SingleCompressedLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::SINGLE_LUT ) {
                    error = SingleLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_LUT ) {
                    error = WalkTreeLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_COMPRESSED_LUT ) {
                    error = WalkTreeCompressedLUT::checkPrecode<5U, 16U>( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_COMPRESSED_SINGLE_LUT ) {
                    error = WalkTreeCompressedLUT::checkPrecode<7U, 512U>( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_COMPRESSED_SINGLE_LUT_2 ) {
                    error = WalkTreeCompressedSingleLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WITHOUT_LUT_USING_ARRAY ) {
                    error = WithoutLUT::checkPrecodeUsingArray( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WITHOUT_LUT ) {
                    error = WithoutLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::VALID_HISTOGRAMS_LUT ) {
                    throw std::logic_error( "Not supported" );
                }

                if ( error != rapidgzip::Error::NONE ) {
                    return false;
                }
            } catch ( const gzip::BitReader::EndOfFileReached& ) {}

            try {
                bitReader.seek( static_cast<long long int>( offset ) + 3 );
                return block.readDynamicHuffmanCoding( bitReader ) == rapidgzip::Error::NONE;
            } catch ( const gzip::BitReader::EndOfFileReached& ) {}
            return false;
        };

    std::copy_if( bitOffsetCandidates.begin(), bitOffsetCandidates.end(),
                  std::back_inserter( bitOffsets ), checkOffset );

    //std::cerr << "    Candidates after second pass: " << bitOffsets.size()
    //          << ", pass took " << duration( t1 ) << " s\n";

    /**
     * Tested with WalkTreeLUT:
     * Candidates after first pass: 43801, pass took 0.161696 s
     * Candidates after second pass: 0, pass took 0.0199207 s
     */

    return bitOffsets;
}


template<uint8_t            CACHED_BIT_COUNT,
         CheckPrecodeMethod CHECK_PRECODE_METHOD = OPTIMAL_CHECK_PRECODE_METHOD>
[[nodiscard]] std::vector<size_t>
findDeflateBlocksRapidgzipLUTTwoPassWithPrecode( BufferedFileReader::AlignedBuffer data )
{
    static_assert( CACHED_BIT_COUNT >= 13,
                   "The LUT must check at least 13-bits, i.e., up to including the distance "
                   "code length check, to avoid duplicate checks in the precode check!" );

    const size_t nBitsToTest = data.size() * CHAR_BIT;
    gzip::BitReader bitReader( std::make_unique<BufferedFileReader>( std::move( data ) ) );

    std::vector<size_t> bitOffsetCandidates;

    using namespace rapidgzip::blockfinder;
    using namespace rapidgzip::deflate;  /* For the definitions of deflate-specific number of bits. */

    const auto oldOffset = bitReader.tell();

    /**
     * For LUT we need at CACHED_BIT_COUNT bits and for the precode check we would need in total
     * 13 + 4 + 57 = 74 bits. Because this does not fit into 64-bit we need to keep two sliding bit buffers.
     * The first can simply have length CACHED_BIT_COUNT and the other one can even keep duplicated bits to
     * have length of 61 bits required for the precode. Updating three different buffers would require more
     * instructions but might not be worth it.
     */
    auto bitBufferForLUT = bitReader.peek<CACHED_BIT_COUNT>();
    bitReader.seek( static_cast<long long int>( oldOffset ) + 13 );
    constexpr auto ALL_PRECODE_BITS = PRECODE_COUNT_BITS + MAX_PRECODE_COUNT * PRECODE_BITS;
    static_assert( ( ALL_PRECODE_BITS == 61 ) && ( ALL_PRECODE_BITS >= CACHED_BIT_COUNT )
                   && ( ALL_PRECODE_BITS <= std::numeric_limits<uint64_t>::digits )
                   && ( ALL_PRECODE_BITS <= gzip::BitReader::MAX_BIT_BUFFER_SIZE ),
                   "It must fit into 64-bit and it also must fit the largest possible jump in the LUT." );
    auto bitBufferPrecodeBits = bitReader.read<ALL_PRECODE_BITS>();

    //const auto t0 = now();
    try {
        for ( size_t offset = oldOffset; offset <= nBitsToTest; ) {
            auto nextPosition = NEXT_DYNAMIC_DEFLATE_CANDIDATE_LUT<CACHED_BIT_COUNT>[bitBufferForLUT];
            if ( nextPosition == 0 ) {
                nextPosition = 1;

                const auto next4Bits = bitBufferPrecodeBits & nLowestBitsSet<uint64_t, PRECODE_COUNT_BITS>();
                const auto next57Bits = ( bitBufferPrecodeBits >> PRECODE_COUNT_BITS )
                                        & nLowestBitsSet<uint64_t, MAX_PRECODE_COUNT * PRECODE_BITS>();

                using namespace rapidgzip::PrecodeCheck;
                auto precodeError = rapidgzip::Error::NONE;
                if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::SINGLE_COMPRESSED_LUT ) {
                    precodeError = SingleCompressedLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::SINGLE_LUT ) {
                    precodeError = SingleLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_LUT ) {
                    precodeError = WalkTreeLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_COMPRESSED_LUT ) {
                    precodeError = WalkTreeCompressedLUT::checkPrecode<5U, 16U>( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_COMPRESSED_SINGLE_LUT ) {
                    precodeError = WalkTreeCompressedLUT::checkPrecode<7U, 512U>( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_COMPRESSED_SINGLE_LUT_2 ) {
                    precodeError = WalkTreeCompressedSingleLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WITHOUT_LUT_USING_ARRAY ) {
                    precodeError = WithoutLUT::checkPrecodeUsingArray( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WITHOUT_LUT ) {
                    precodeError = WithoutLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::VALID_HISTOGRAMS_LUT ) {
                    throw std::logic_error( "Not supported" );
                }

                if ( UNLIKELY( precodeError == rapidgzip::Error::NONE ) ) [[unlikely]] {
                    bitOffsetCandidates.push_back( offset );
                }
            }

            const auto bitsToLoad = nextPosition;

            /* Refill bit buffer for LUT using the bits from the higher precode bit buffer. */
            bitBufferForLUT >>= bitsToLoad;
            if constexpr ( CACHED_BIT_COUNT > 13 ) {
                constexpr uint8_t DUPLICATED_BITS = CACHED_BIT_COUNT - 13;
                bitBufferForLUT |= ( ( bitBufferPrecodeBits >> DUPLICATED_BITS )
                                     & nLowestBitsSet<uint64_t>( bitsToLoad ) )
                                   << static_cast<uint8_t>( CACHED_BIT_COUNT - bitsToLoad );
            } else {
                bitBufferForLUT |= ( bitBufferPrecodeBits & nLowestBitsSet<uint64_t>( bitsToLoad ) )
                                   << static_cast<uint8_t>( CACHED_BIT_COUNT - bitsToLoad );
            }

            /* Refill the precode bit buffer directly from the bit reader. */
            bitBufferPrecodeBits >>= bitsToLoad;
            bitBufferPrecodeBits |= bitReader.read( bitsToLoad )
                                    << static_cast<uint8_t>( ALL_PRECODE_BITS - bitsToLoad );

            offset += nextPosition;
        }
    } catch ( const gzip::BitReader::EndOfFileReached& ) {
        /* Might happen when testing close to the end. */
    }

    //const auto t1 = now();
    //std::cerr << "    Candidates after first pass: " << bitOffsetCandidates.size()
    //          << ", pass took " << duration( t0, t1 ) << " s\n";

    std::vector<size_t> bitOffsets;

    rapidgzip::deflate::Block block;

    const auto checkOffset =
        [&] ( const auto offset )
        {
            /* Check the precode Huffman coding. We can skip a lot of the generic tests done in deflate::Block
             * because this is only called for offsets prefiltered by the LUT. But, this also means that the
             * LUT size must be at least 13-bit! */
            try {
                bitReader.seek( static_cast<long long int>( offset ) + 13 );
                const auto next4Bits = bitReader.read( rapidgzip::deflate::PRECODE_COUNT_BITS );
                const auto next57Bits = bitReader.peek( rapidgzip::deflate::MAX_PRECODE_COUNT
                                                        * rapidgzip::deflate::PRECODE_BITS );
                static_assert( rapidgzip::deflate::MAX_PRECODE_COUNT * rapidgzip::deflate::PRECODE_BITS
                               <= gzip::BitReader::MAX_BIT_BUFFER_SIZE,
                               "This optimization requires a larger BitBuffer inside BitReader!" );

                using rapidgzip::PrecodeCheck::WalkTreeLUT::checkPrecode;
                const auto error = checkPrecode( next4Bits, next57Bits );

                if ( error != rapidgzip::Error::NONE ) {
                    return false;
                }
            } catch ( const gzip::BitReader::EndOfFileReached& ) {}

            try {
                bitReader.seek( static_cast<long long int>( offset ) + 3 );
                return block.readDynamicHuffmanCoding( bitReader ) == rapidgzip::Error::NONE;
            } catch ( const gzip::BitReader::EndOfFileReached& ) {}
            return false;
        };

    std::copy_if( bitOffsetCandidates.begin(), bitOffsetCandidates.end(),
                  std::back_inserter( bitOffsets ), checkOffset );

    //std::cerr << "    Candidates after second pass: " << bitOffsets.size()
    //          << ", pass took " << duration( t1 ) << " s\n";

    /**
     * Tested with WalkTreeLUT:
     * Candidates after first pass: 43801, pass took 0.16743 s
     * Candidates after second pass: 0, pass took 0.0216388 s
     */
    return bitOffsets;
}


[[nodiscard]] std::vector<size_t>
findUncompressedDeflateBlocksNestedBranches( const BufferedFileReader::AlignedBuffer& buffer )
{
    std::vector<size_t> bitOffsets;

    for ( size_t i = 2; i + 2 < buffer.size(); ++i ) {
        if ( LIKELY( static_cast<uint8_t>( static_cast<uint8_t>( buffer[i] )
                                           ^ static_cast<uint8_t>( buffer[i + 2] ) ) != 0xFFU ) ) [[likely]] {
            continue;
        }

        if ( LIKELY( static_cast<uint8_t>( static_cast<uint8_t>( buffer[i - 1] )
                                           ^ static_cast<uint8_t>( buffer[i + 1] ) ) != 0xFFU ) ) [[likely]] {
            continue;
        }

        if ( LIKELY( ( static_cast<uint8_t>( buffer[i - 2] ) & 0b111U ) != 0 ) ) [[likely]] {
            continue;
        }

        if ( UNLIKELY( ( buffer[i] == 0 ) && ( buffer[i - 1] == 0 ) ) ) [[unlikely]] {
            continue;
        }

        /* The size and negated size must be preceded by at least three zero bits, one indicating a non-final block
         * and two indicating a non-compressed block. This test assumes that the padding between the deflate block
         * header and the byte-aligned non-compressed data is zero!
         * @todo It is fine ignoring weird output with non-zero padding in the finder but the decoder should then
         *       know of this and not stop decoding thinking that the other thread has found that block!
         * @todo I might need an interface to determine what blocks could have been found and what not :/ */
        uint8_t trailingZeros = 3;
        for ( uint8_t j = trailingZeros + 1; j <= 8U; ++j ) {
            if ( ( static_cast<uint8_t>( buffer[i - 1] ) & ( 1U << static_cast<uint8_t>( j - 1U ) ) ) == 0 ) {
                trailingZeros = j;
            }
        }
        bitOffsets.push_back( i * CHAR_BIT - trailingZeros );
    }

    return bitOffsets;
}


[[nodiscard]] std::vector<size_t>
findUncompressedDeflateBlocks( const BufferedFileReader::AlignedBuffer& buffer )
{
    std::vector<size_t> bitOffsets;

    for ( size_t i = 1; i + 2 < buffer.size(); ++i ) {
        const auto blockSize = loadUnaligned<uint16_t>( buffer.data() + i );
        const auto negatedBlockSize = loadUnaligned<uint16_t>( buffer.data() + i + 2 );
        if ( LIKELY( static_cast<uint16_t>( blockSize ^ negatedBlockSize ) != 0xFFFFU ) ) [[likely]] {
            continue;
        }

        if ( LIKELY( ( static_cast<uint8_t>( buffer[i - 1] ) & 0b111U ) != 0 ) ) [[likely]] {
            continue;
        }

        if ( UNLIKELY( blockSize == 0 ) ) {
            continue;
        }

        uint8_t trailingZeros = 3;
        for ( uint8_t j = trailingZeros + 1; j <= 8; ++j ) {
            if ( ( static_cast<uint8_t>( buffer[i - 1] ) & ( 1U << static_cast<uint8_t>( j - 1U ) ) ) == 0 ) {
                trailingZeros = j;
            }
        }

        bitOffsets.push_back( i * CHAR_BIT - trailingZeros );
    }

    return bitOffsets;
}


[[nodiscard]] BufferedFileReader::AlignedBuffer
bufferFile( const std::string& fileName,
            size_t             bytesToBuffer = std::numeric_limits<size_t>::max() )
{
    const auto file = throwingOpen( fileName, "rb" );
    BufferedFileReader::AlignedBuffer buffer( std::min( fileSize( fileName ), bytesToBuffer ), 0 );
    const auto nElementsReadFromFile = std::fread( buffer.data(), sizeof( buffer[0] ), buffer.size(), file.get() );
    buffer.resize( nElementsReadFromFile );
    return buffer;
}


[[nodiscard]] std::string
formatBandwidth( const std::vector<double>& times,
                 size_t                     byteCount )
{
    std::vector<double> bandwidths( times.size() );
    std::transform( times.begin(), times.end(), bandwidths.begin(),
                    [byteCount] ( double time ) { return static_cast<double>( byteCount ) / time / 1e6; } );
    Statistics<double> bandwidthStats{ bandwidths };

    /* Motivation for showing min times and maximum bandwidths are because nothing can go faster than
     * physically possible but many noisy influences can slow things down, i.e., the minimum time is
     * the value closest to be free of noise. */
    std::stringstream result;
    result << "( " + bandwidthStats.formatAverageWithUncertainty( true ) << " ) MB/s";
    return result.str();
}


void
benchmarkGzip( const std::string& fileName )
{
    {
        const auto buffer = bufferFile( fileName, 128_Mi );
        const auto [blockCandidates, durations] = benchmarkFunction<10>(
            [&buffer] () { return findUncompressedDeflateBlocks( buffer ); } );

        std::cout << "[findUncompressedDeflateBlocks] " << formatBandwidth( durations, buffer.size() ) << "\n";
        std::cout << "    Block candidates (" << blockCandidates.size() << "): " << blockCandidates << "\n\n";
    }

    {
        const auto buffer = bufferFile( fileName, 128_Mi );
        const auto [blockCandidates, durations] = benchmarkFunction<10>(
            [&buffer] () { return findUncompressedDeflateBlocksNestedBranches( buffer ); } );

        std::cout << "[findUncompressedDeflateBlocksNestedBranches] "
                  << formatBandwidth( durations, buffer.size() ) << "\n";
        std::cout << "    Block candidates (" << blockCandidates.size() << "): "
                  << blockCandidates << "\n\n";
    }

    /* Ground truth offsets. */
    const auto [streamOffsets, blockOffsets] = parseWithZlib( fileName );
    std::cout << "Gzip streams (" << streamOffsets.size() << "): " << streamOffsets << "\n";
    std::cout << "Deflate blocks (" << blockOffsets.size() << "): " << blockOffsets << "\n\n";

    /* Print block size information */
    {
        std::vector<size_t> blockSizes( blockOffsets.size() );
        std::adjacent_difference( blockOffsets.begin(), blockOffsets.end(), blockSizes.begin() );
        assert( !blockSizes.empty() );
        blockSizes.erase( blockSizes.begin() );  /* adjacent_difference begins writing at output begin + 1! */

        const Histogram<size_t> sizeHistogram{ blockSizes, 6, "b" };

        std::cout << "Block size distribution: min: " << sizeHistogram.statistics().min / CHAR_BIT << " B"
                  << ", avg: " << sizeHistogram.statistics().average() / CHAR_BIT << " B"
                  << " +- " << sizeHistogram.statistics().standardDeviation() / CHAR_BIT << " B"
                  << ", max: " << sizeHistogram.statistics().max / CHAR_BIT << " B\n";

        std::cout << "Block Size Distribution (small to large):\n" << sizeHistogram.plot() << "\n\n";
    }

    /* In general, all solutions should return all blocks except for the final block, uncompressed blocks
     * and fixed Huffman encoded blocks. */
    const auto verifyCandidates =
        [&blockOffsets2 = blockOffsets]
        ( const std::vector<size_t>& blockCandidates,
          const size_t               nBytesToTest )
        {
            for ( size_t i = 0; i + 1 < blockOffsets2.size(); ++i ) {
                /* Pigz produces a lot of very small fixed Huffman blocks, probably because of a "flush".
                 * But the block finder don't have to find fixed Huffman blocks */
                const auto size = blockOffsets2[i + 1] - blockOffsets2[i];
                if ( size < 1000 ) {
                    continue;
                }

                /* Especially for the naive zlib finder up to one deflate block might be missing,
                 * i.e., up to ~64 KiB! */
                const auto offset = blockOffsets2[i];
                if ( offset >= nBytesToTest * CHAR_BIT - 128_Ki * CHAR_BIT ) {
                    break;
                }

                if ( !contains( blockCandidates, offset ) ) {
                    std::stringstream message;
                    message << "Block " << i << " at offset " << offset << " was not found!";
                    throw std::logic_error( std::move( message ).str() );
                }
            }

            if ( blockCandidates.size() > 2 * blockOffsets2.size() + 10 ) {
                throw std::logic_error( "Too many false positives found!" );
            }
        };

    {
        const auto buffer = bufferFile( fileName, 256_Ki );
        const auto [blockCandidates, durations] = benchmarkFunction<10>(
            [&buffer] () { return findDeflateBlocksZlib( buffer ); } );

        std::cout << "[findDeflateBlocksZlib] " << formatBandwidth( durations, buffer.size() ) << "\n";
        std::cout << "    Block candidates (" << blockCandidates.size() << "): "
                  << blockCandidates << "\n\n";
        verifyCandidates( blockCandidates, buffer.size() );
    }

    /* Because final blocks are skipped, it won't find anything for BGZ files! */
    const auto isBgzfFile =
        rapidgzip::blockfinder::Bgzf::isBgzfFile( std::make_unique<StandardFileReader>( fileName ) );
    if ( !isBgzfFile ) {
        const auto buffer = bufferFile( fileName, 256_Ki );
        const auto [blockCandidates, durations] = benchmarkFunction<10>(
            [&buffer] () { return findDeflateBlocksZlibOptimized( buffer ); } );

        std::cout << "[findDeflateBlocksZlibOptimized] " << formatBandwidth( durations, buffer.size() ) << "\n";
        std::cout << "    Block candidates (" << blockCandidates.size() << "): "
                  << blockCandidates << "\n\n";
    }

    /* Benchmarks with own implementation (rapidgzip). */
    {
        static constexpr auto OPTIMAL_NEXT_DEFLATE_LUT_SIZE = rapidgzip::blockfinder::OPTIMAL_NEXT_DEFLATE_LUT_SIZE;
        const auto buffer = bufferFile( fileName, 16_Mi );

        const auto blockCandidates = countFilterEfficiencies( buffer );
        std::cout << "Block candidates (" << blockCandidates.size() << "): " << blockCandidates << "\n\n";

        const auto [blockCandidatesRapidgzip, durations] = benchmarkFunction<10>(
            [&buffer] () { return findDeflateBlocksRapidgzip( buffer ); } );

        if ( blockCandidates != blockCandidatesRapidgzip ) {
            std::stringstream msg;
            msg << "Results with findDeflateBlocksRapidgzip differ! Block candidates ("
                << blockCandidatesRapidgzip.size() << "): " << blockCandidatesRapidgzip;
            throw std::logic_error( std::move( msg ).str() );
        }
        std::cout << std::setw( 37 ) << std::left << "[findDeflateBlocksRapidgzip] " << std::right
                  << formatBandwidth( durations, buffer.size() ) << "\n";

        /* Same as above but with a LUT that can skip bits similar to the Boyer–Moore string-search algorithm. */

        /* Call findDeflateBlocksRapidgzipLUT once to initialize the static variables! */
        if ( const auto blockCandidatesLUT = findDeflateBlocksRapidgzipLUT<OPTIMAL_NEXT_DEFLATE_LUT_SIZE>( buffer );
             blockCandidatesLUT != blockCandidates ) {
            std::stringstream msg;
            msg << "Results with findDeflateBlocksRapidgzipLUT differ! Block candidates ("
                << blockCandidatesLUT.size() << "): " << blockCandidatesLUT;
            throw std::logic_error( std::move( msg ).str() );
        }

        const auto [blockCandidatesLUT, durationsLUT] = benchmarkFunction<10>(
            /* As for choosing CACHED_BIT_COUNT == 13, see the output of the results at the end of the file.
             * 13 is the last for which it significantly improves over less bits and 14 bits produce reproducibly
             * slower bandwidths! 13 bits is the best configuration as far as I know. */
            [&buffer] () { return findDeflateBlocksRapidgzipLUT<OPTIMAL_NEXT_DEFLATE_LUT_SIZE>( buffer ); } );

        if ( blockCandidates != blockCandidatesLUT ) {
            std::stringstream msg;
            msg << "Results with findDeflateBlocksRapidgzipLUT differ! Block candidates ("
                << blockCandidatesLUT.size() << "): " << blockCandidatesLUT;
            throw std::logic_error( std::move( msg ).str() );
        }
        std::cout << std::setw( 37 ) << std::left << "[findDeflateBlocksRapidgzipLUT] " << std::right
                  << formatBandwidth( durationsLUT, buffer.size() ) << "\n";

        /* Same as above but with a LUT and two-pass. */

        const auto [blockCandidatesLUT2P, durationsLUT2P] = benchmarkFunction<10>(
            /* As for choosing CACHED_BIT_COUNT == 13, see the output of the results at the end of the file.
             * 13 is the last for which it significantly improves over less bits and 14 bits produce reproducibly
             * slower bandwidths! 13 bits is the best configuration as far as I know. */
            [&buffer] () { return findDeflateBlocksRapidgzipLUTTwoPass<OPTIMAL_NEXT_DEFLATE_LUT_SIZE>( buffer ); } );

        if ( blockCandidates != blockCandidatesLUT2P ) {
            std::stringstream msg;
            msg << "Results with findDeflateBlocksRapidgzipLUTTwoPass differ! Block candidates ("
                << blockCandidatesLUT2P.size() << "): " << blockCandidatesLUT2P;
            throw std::logic_error( std::move( msg ).str() );
        }
        std::cout << "[findDeflateBlocksRapidgzipLUTTwoPass] "
                  << formatBandwidth( durationsLUT2P, buffer.size() ) << "\n";
    }

    if ( isBgzfFile ) {
        const auto [blockCandidates, durations] =
            benchmarkFunction<10>( [fileName] () { return findBgzStreams( fileName ); } );

        std::cout << "[findBgzStreams] " << formatBandwidth( durations, fileSize( fileName ) ) << "\n";
        std::cout << "    Block candidates (" << blockCandidates.size() << "): "
                  << blockCandidates << "\n\n";
    }

    {
        const auto gzipStreams = findGzipStreams( fileName );
        if ( !gzipStreams.empty() ) {
            std::cout << "Found " << gzipStreams.size() << " gzip stream candidates!\n" << gzipStreams << "\n\n";
        }
    }

    std::cout << "\n";
}


template<uint8_t CACHED_BIT_COUNT>
uint64_t
benchmarkLUTSizeOnlySkipManualSlidingBufferLUT( const BufferedFileReader::AlignedBuffer& buffer )
{
    /* As long as we lookup more or equal than 13 bits, we should get the exact same candidates because only
     * the first 13 bits are actually checked, higher lookups only allow for conflating double skips! */
    std::optional<uint64_t> alternativeCandidateCount;
    if constexpr ( CACHED_BIT_COUNT > 13 ) {
        alternativeCandidateCount = benchmarkLUTSizeOnlySkipManualSlidingBufferLUT<CACHED_BIT_COUNT - 1>( buffer );
    }

    const auto [candidateCount, durations] = benchmarkFunction<10>(
        [&buffer] () { return countDeflateBlocksPreselectionManualSlidingBuffer<CACHED_BIT_COUNT>( buffer ); } );

    std::cout << "[findDeflateBlocksRapidgzipLUT with " << static_cast<int>( CACHED_BIT_COUNT ) << " bits] "
              << formatBandwidth( durations, buffer.size() ) << " (" << candidateCount << " candidates)\n";

    if ( alternativeCandidateCount && ( *alternativeCandidateCount != candidateCount ) ) {
        throw std::logic_error( "Got inconsistent number of candidates for deflate blockfinder with "
                                "different LUT table sizes!" );
    }

    return candidateCount;
}


template<uint8_t CACHED_BIT_COUNT>
uint64_t
benchmarkLUTSizeOnlySkipLUT( const BufferedFileReader::AlignedBuffer& buffer )
{
    /* As long as we lookup more or equal than 13 bits, we should get the exact same candidates because only
     * the first 13 bits are actually checked, higher lookups only allow for conflating double skips! */
    std::optional<uint64_t> alternativeCandidateCount;
    if constexpr ( CACHED_BIT_COUNT > 13 ) {
        alternativeCandidateCount = benchmarkLUTSizeOnlySkipLUT<CACHED_BIT_COUNT - 1>( buffer );
    }

    const auto [candidateCount, durations] = benchmarkFunction<10>(
        [&buffer] () { return countDeflateBlocksPreselection<CACHED_BIT_COUNT>( buffer ); } );

    std::cout << "[findDeflateBlocksRapidgzipLUT with " << static_cast<int>( CACHED_BIT_COUNT ) << " bits] "
              << formatBandwidth( durations, buffer.size() ) << " (" << candidateCount << " candidates)\n";

    if ( alternativeCandidateCount && ( *alternativeCandidateCount != candidateCount ) ) {
        throw std::logic_error( "Got inconsistent number of candidates for deflate blockfinder with "
                                "different LUT table sizes!" );
    }
    return candidateCount;
}


enum class FindDeflateMethod
{
    FULL_CHECK,
    TWO_PASS,
    TWO_PASS_WITH_PRECODE,
};


[[nodiscard]] std::string
toString( FindDeflateMethod method )
{
    switch ( method )
    {
    case FindDeflateMethod::FULL_CHECK            : return "findDeflateBlocksRapidgzipLUT";
    case FindDeflateMethod::TWO_PASS              : return "findDeflateBlocksRapidgzipLUTTwoPass";
    case FindDeflateMethod::TWO_PASS_WITH_PRECODE : return "findDeflateBlocksRapidgzipLUTTwoPassAndPrecode";
    }
    throw std::invalid_argument( "Unknown find deflate method!" );
}


template<uint8_t            CACHED_BIT_COUNT,
         FindDeflateMethod  FIND_DEFLATE_METHOD,
         CheckPrecodeMethod CHECK_PRECODE_METHOD>
std::vector<size_t>
benchmarkLUTSize( const BufferedFileReader::AlignedBuffer& buffer )
{
    std::optional<std::vector<size_t> > blockCandidatesWithLessBits;
    if constexpr ( CACHED_BIT_COUNT > 13 ) {
        blockCandidatesWithLessBits =
            benchmarkLUTSize<CACHED_BIT_COUNT - 1, FIND_DEFLATE_METHOD, CHECK_PRECODE_METHOD>( buffer );
    }

    const auto [blockCandidates, durations] = benchmarkFunction<10>(
        [&buffer] () {
            if constexpr ( FIND_DEFLATE_METHOD == FindDeflateMethod::FULL_CHECK ) {
                return findDeflateBlocksRapidgzipLUT<CACHED_BIT_COUNT, CHECK_PRECODE_METHOD>( buffer );
            } else if constexpr ( FIND_DEFLATE_METHOD == FindDeflateMethod::TWO_PASS ) {
                return findDeflateBlocksRapidgzipLUTTwoPass<CACHED_BIT_COUNT, CHECK_PRECODE_METHOD>( buffer );
            } else if constexpr ( FIND_DEFLATE_METHOD == FindDeflateMethod::TWO_PASS_WITH_PRECODE ) {
                return findDeflateBlocksRapidgzipLUTTwoPassWithPrecode<CACHED_BIT_COUNT,
                                                                       CHECK_PRECODE_METHOD>( buffer );
            }
        } );

    std::cout << "[" << toString( FIND_DEFLATE_METHOD ) << " with " << static_cast<int>( CACHED_BIT_COUNT ) << " bits, "
              << toString( CHECK_PRECODE_METHOD ) << "] "
              << formatBandwidth( durations, buffer.size() ) << " (candidates: " << blockCandidates.size() << ")\n";

    if ( blockCandidatesWithLessBits && ( *blockCandidatesWithLessBits != blockCandidates ) ) {
        std::cerr << "blockCandidatesWithLessBits (" << blockCandidatesWithLessBits->size() << "):"
                  << *blockCandidatesWithLessBits << "\n"
                  << "blockCandidates (" << blockCandidates.size() << "):" << blockCandidates << "\n";
        throw std::logic_error( "Got inconsistent block candidates for rapidgzip blockfinder with different "
                                "LUT table sizes!" );
    }

    return blockCandidates;
}


template<uint8_t           CACHED_BIT_COUNT,
         FindDeflateMethod FIND_METHOD>
void
benchmarkLUTSizeAllCheckPrecodeMethods( const BufferedFileReader::AlignedBuffer& buffer )
{
    using CPM = CheckPrecodeMethod;

    /* Roughly sorted such that the fastest come first, so that slow implementations do not make us wait
     * unnecessarily. Furthermore, the fastest implementations are likely the most recent and work-in-progress ones. */
    benchmarkLUTSize<CACHED_BIT_COUNT, FIND_METHOD, CPM::WALK_TREE_COMPRESSED_SINGLE_LUT_2>( buffer );  // ~80 MB/s
    std::cout << "\n";
    benchmarkLUTSize<CACHED_BIT_COUNT, FIND_METHOD,
                     CPM::WALK_TREE_COMPRESSED_SINGLE_LUT>( buffer );  // ~80 MB/S
    std::cout << "\n";
    benchmarkLUTSize<CACHED_BIT_COUNT, FIND_METHOD, CPM::WALK_TREE_LUT>( buffer );  // ~75 MB/S
    std::cout << "\n";
    benchmarkLUTSize<CACHED_BIT_COUNT, FIND_METHOD, CPM::WALK_TREE_COMPRESSED_LUT>( buffer );  // ~70 MB/S
    std::cout << "\n";
    benchmarkLUTSize<CACHED_BIT_COUNT, FIND_METHOD, CPM::SINGLE_LUT>( buffer );  // ~90 MB/s
    std::cout << "\n";
    benchmarkLUTSize<CACHED_BIT_COUNT, FIND_METHOD, CPM::SINGLE_COMPRESSED_LUT>( buffer );  // ~90 MB/s
    std::cout << "\n";
    benchmarkLUTSize<CACHED_BIT_COUNT, FIND_METHOD, CPM::WITHOUT_LUT_USING_ARRAY>( buffer );  // ~45 MB/s
    std::cout << "\n";
    benchmarkLUTSize<CACHED_BIT_COUNT, FIND_METHOD, CPM::WITHOUT_LUT>( buffer );  // ~45 MB/s
    std::cout << "\n";
}


template<uint8_t MIN_CACHED_BIT_COUNT,
         uint8_t MAX_CACHED_BIT_COUNT,
         uint8_t CACHED_BIT_COUNT = MIN_CACHED_BIT_COUNT>
void
analyzeDeflateJumpLUT()
{
    using namespace rapidgzip::blockfinder;
    static const auto LUT = NEXT_DYNAMIC_DEFLATE_CANDIDATE_LUT<CACHED_BIT_COUNT>;

    std::cerr << "Deflate Jump LUT for " << static_cast<int>( CACHED_BIT_COUNT ) << " bits is sized: "
              << formatBytes( LUT.size() * sizeof( LUT[0] ) ) << " with the following jump distance distribution:\n";
    std::map<uint32_t, uint64_t> jumpFrequencies;
    for ( const auto x : LUT ) {
        jumpFrequencies[x]++;
    }
    for ( const auto& [distance, count] : jumpFrequencies ) {
        if ( count > 0 ) {
            std::cerr << "    " << std::setw( 2 ) << distance << " : " << std::setw( 5 ) << count << " ("
                      << static_cast<double>( count ) / static_cast<double>( LUT.size() ) * 100 << " %)\n";
        }
    }
    std::cerr << "\n";

    if constexpr ( CACHED_BIT_COUNT < MAX_CACHED_BIT_COUNT ) {
        analyzeDeflateJumpLUT<MIN_CACHED_BIT_COUNT, MAX_CACHED_BIT_COUNT, CACHED_BIT_COUNT + 1>();
    }
}


void
printLUTSizes()
{
    using namespace rapidgzip;
    std::cerr << "VALID_HUFFMAN_CODINGS                     : "  // 488320 B
              << formatBytes( sizeof( deflate::precode::VALID_HUFFMAN_CODINGS ) ) << "\n";
    std::cerr << "CRC32LookupTable                          : " << sizeof( CRC32_TABLE ) << "\n";  // 1 KiB
    std::cerr << "CRC32_SLICE_BY_N_LUT                      : " << sizeof( CRC32_SLICE_BY_N_LUT ) << "\n";  // 64 KiB
    std::cerr << "NEXT_DEFLATE_CANDIDATE_LUTS_UP_TO_13_BITS : "  // 16 KiB
              << formatBytes( sizeof( blockfinder::NEXT_DEFLATE_CANDIDATE_LUTS_UP_TO_13_BITS ) ) << "\n";
    std::cerr << "NEXT_DYNAMIC_DEFLATE_CANDIDATE_LUT<"
              << static_cast<int>( blockfinder::OPTIMAL_NEXT_DEFLATE_LUT_SIZE ) << ">    : "  // 16 KiB
              << formatBytes( sizeof(
                     blockfinder::NEXT_DYNAMIC_DEFLATE_CANDIDATE_LUT<blockfinder::OPTIMAL_NEXT_DEFLATE_LUT_SIZE> ) )
              << "\n";
    std::cerr << "PRECODE_FREQUENCIES_1_TO_5_VALID_LUT      : "  // 4 MiB
              << formatBytes( sizeof( PrecodeCheck::WalkTreeLUT::PRECODE_FREQUENCIES_1_TO_5_VALID_LUT ) ) << "\n";
    std::cerr << "PRECODE_TO_FREQUENCIES_LUT<4>             : "  // 32 KiB
              << formatBytes( sizeof( PrecodeCheck::WalkTreeLUT::PRECODE_TO_FREQUENCIES_LUT<4> ) ) << "\n";
    std::cerr << "HISTOGRAM_TO_ID_LUT                       : "  // 216 KiB
              << formatBytes( sizeof( PrecodeCheck::SingleLUT::ValidHistogramID::HISTOGRAM_TO_ID_LUT ) ) << "\n";
    std::cerr << "PRECODE_HISTOGRAM_VALID_LUT               : "  // 2 MiB
              << formatBytes( sizeof( PrecodeCheck::SingleLUT::PRECODE_HISTOGRAM_VALID_LUT ) ) << "\n";
}


int
main( int    argc,
      char** argv )
{
    std::srand( 0x19AAA8FDU );

    if ( argc > 1 ) {
        for ( int i = 1; i < argc; ++i ) {
            if ( std::filesystem::exists( argv[i] ) ) {
                benchmarkGzip( argv[i] );
            }
        }
    }

    printLUTSizes();

    const auto tmpFolder = createTemporaryDirectory( "rapidgzip.benchmarkGzipBlockFinder" );
    const std::string fileName = std::filesystem::absolute( tmpFolder.path() / "random-base64" );

    const std::vector<std::tuple<std::string, std::string, std::string, std::string > > testEncoders = {
        { "gzip", "gzip --version", "gzip -k --force", "gzip" },
        { "pigz", "pigz --version", "pigz -k --force", "pigz" },
        { "igzip", "igzip --version", "igzip -k --force", "igzip" },
        { "bgzip", "bgzip --version", "bgzip --force", "bgzip" },
        { "Python3 gzip", "python3 --version", "python3 -m gzip", "python3-gzip" },
        { "Python3 pgzip", "python3 -m pip show pgzip", "python3 -m pgzip", "python3-pgzip" },
    };

    try
    {
        for ( const auto& [name, getVersion, command, extension] : testEncoders ) {
            /* Check for the uncompressed file inside the loop because "bgzip" does not have a --keep option!
             * https://github.com/samtools/htslib/pull/1331 */
            if ( !std::filesystem::exists( fileName ) ) {
                createRandomBase64( fileName, 16_Mi );
            }

            /* Python3 module pgzip does not create the .gz file beside the input file but in the current directory,
             * so change current directory to the input file first. */
            const auto oldCWD = std::filesystem::current_path();
            std::filesystem::current_path( tmpFolder );

            const auto fullCommand = command + " " + fileName;
            const auto returnCode = std::system( fullCommand.c_str() );

            std::filesystem::current_path( oldCWD );

            if ( returnCode != 0 ) {
                std::cerr << "Failed to encode the temporary file with: " << fullCommand << "\n";
                continue;
            }

            if ( !std::filesystem::exists( fileName + ".gz" ) ) {
                std::cerr << "Encoded file was not found!\n";
                continue;
            }

            const auto newFileName = fileName + "." + extension;
            std::filesystem::rename( fileName + ".gz", newFileName );


            /* Benchmark Rapidgzip LUT version with different LUT sizes. */

            if ( name == "gzip" ) {
                const auto data = bufferFile( newFileName );

                /* CACHED_BIT_COUNT == 19 fails on GCC because it requires > 99 M constexpr steps.
                 * CACHED_BIT_COUNT == 18 fail on clang because it requires > 99 M constexpr steps.
                 * It works when using simple const instead of constexpr.
                 * This is a maximum cached bit count. It will benchmark all the way down to 13. */
                constexpr auto MAX_CACHED_BIT_COUNT = 18U;

                /* Force the static LUT to be instantiated here to not affect the benchmarks. */
                if ( PrecodeCheck::WalkTreeCompressedSingleLUT::checkPrecode( 0, 1 ) != rapidgzip::Error::NONE ) {
                    throw std::logic_error( "A single non-zero precode code length should be valid." );
                }

            /* Do not always compile and run all tests because it increases compile-time and runtime a lot. */
            //#define BENCHMARK_ALL_VERSIONS_WITH_DIFFERENT_JUMP_LUT_SIZES
            #ifdef BENCHMARK_ALL_VERSIONS_WITH_DIFFERENT_JUMP_LUT_SIZES

                std::cout << "== Testing different rapidgzip deflate header jump LUT table sizes ==\n\n";
                std::cout << "=== Only using the skip LUT (many false positives) and manual sliding bit buffer ===\n\n";
                const auto candidateCountManualSkipping =
                    benchmarkLUTSizeOnlySkipManualSlidingBufferLUT<MAX_CACHED_BIT_COUNT>( data );
                std::cout << "\n\n";

                std::cout << "=== Only using the skip LUT (many false positives) ===\n\n";
                const auto candidateCountSkipLUTOnly = benchmarkLUTSizeOnlySkipLUT<MAX_CACHED_BIT_COUNT>( data );
                std::cout << "\n\n";

                REQUIRE_EQUAL( candidateCountManualSkipping, candidateCountSkipLUTOnly );

                std::cout << "=== Full test and precode check ===\n\n";
                constexpr auto FULL_CHECK = FindDeflateMethod::FULL_CHECK;
                benchmarkLUTSizeAllCheckPrecodeMethods<MAX_CACHED_BIT_COUNT, FULL_CHECK>( data );
                benchmarkLUTSize<MAX_CACHED_BIT_COUNT, FULL_CHECK, CheckPrecodeMethod::VALID_HISTOGRAMS_LUT>( data );
                std::cout << "\n\n";

                std::cout << "=== Full test and precode check in two passes ===\n\n";
                benchmarkLUTSizeAllCheckPrecodeMethods<MAX_CACHED_BIT_COUNT, FindDeflateMethod::TWO_PASS>( data );
                std::cout << "\n";

                std::cout << "=== Full test and precode check in two passes and precode check in first pass ===\n\n";
                benchmarkLUTSizeAllCheckPrecodeMethods<MAX_CACHED_BIT_COUNT,
                                                       FindDeflateMethod::TWO_PASS_WITH_PRECODE>( data );
                std::cout << "\n";
            #else
                std::cout << "=== Full test and precode check ===\n\n";
                benchmarkLUTSize<MAX_CACHED_BIT_COUNT,
                                 FindDeflateMethod::FULL_CHECK,
                                 CheckPrecodeMethod::SINGLE_LUT>( data );
                benchmarkLUTSize<MAX_CACHED_BIT_COUNT,
                                 FindDeflateMethod::FULL_CHECK,
                                 CheckPrecodeMethod::SINGLE_COMPRESSED_LUT>( data );
                benchmarkLUTSize<MAX_CACHED_BIT_COUNT,
                                 FindDeflateMethod::FULL_CHECK,
                                 CheckPrecodeMethod::WALK_TREE_COMPRESSED_SINGLE_LUT_2>( data );
            #endif
            #undef BENCHMARK_ALL_VERSIONS_WITH_DIFFERENT_JUMP_LUT_SIZES
            }

            /* Benchmark all different blockfinder implementations with the current encoded file. */

            std::cout << "=== Testing with encoder: " << name << " ===\n\n";

            std::cout << "> " << getVersion << "\n";
            [[maybe_unused]] const auto versionReturnCode = std::system( ( getVersion + " > out" ).c_str() );
            std::cout << std::ifstream( "out" ).rdbuf();
            std::cout << "\n";

            benchmarkGzip( newFileName );
        }
    } catch ( const std::exception& exception ) {
        /* Note that the destructor for TemporaryDirectory might not be called for uncaught exceptions!
         * @see https://stackoverflow.com/questions/222175/why-destructor-is-not-called-on-exception */
        std::cerr << "Caught exception: " << exception.what() << "\n";
        return 1;
    }

    analyzeDeflateJumpLUT<13, 18>();

    return 0;
}


/*
( set -o pipefail; cmake --build . -- benchmarkGzipBlockFinder 2>&1 | tee build.log ) &&
stdbuf -o0 -e0 taskset 0x08 src/benchmarks/benchmarkGzipBlockFinder | tee benchmark.log
mv benchmark.log ../results/benchmarks/benchmarkGzipBlockFinder.log
*/
