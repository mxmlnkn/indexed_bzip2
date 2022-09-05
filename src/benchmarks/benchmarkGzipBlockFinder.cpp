/*
DEFLATE Compressed Data Format Specification version 1.3
https://www.rfc-editor.org/rfc/rfc1951.txt

GZIP file format specification version 4.3
https://www.ietf.org/rfc/rfc1952.txt
*/


#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <zlib.h>

#include <BitReader.hpp>
#include <blockfinder/Bgzf.hpp>
#include <blockfinder/DynamicHuffman.hpp>
#include <blockfinder/PigzParallel.hpp>
#include <blockfinder/precodecheck/SingleCompressedLUT.hpp>
#include <blockfinder/precodecheck/SingleLUT.hpp>
#include <blockfinder/precodecheck/WalkTreeCompressedLUT.hpp>
#include <blockfinder/precodecheck/WalkTreeLUT.hpp>
#include <blockfinder/precodecheck/WithoutLUT.hpp>
#include <common.hpp>
#include <filereader/Buffered.hpp>
#include <pragzip.hpp>
#include <Statistics.hpp>
#include <TestHelpers.hpp>


std::ostream&
operator<<( std::ostream& out, std::vector<size_t> vector )
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
        pragzip::blockfinder::Bgzf blockFinder( std::make_unique<StandardFileReader>( fileName ) );

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
    while( true )
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
    GzipWrapper& operator=( GzipWrapper&& ) = delete;
    GzipWrapper& operator=( GzipWrapper& ) = delete;

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

        const auto outputPreviouslyAvailable = std::min( 8_Ki, m_outputBuffer.size() );
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
        if ( gzip.tryInflate( reinterpret_cast<unsigned char*>( buffer.data() ),
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
    pragzip::BitReader bitReader( std::make_unique<BufferedFileReader>( buffer ) );

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
    size_t zlibTestCount = 0;

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
        ++zlibTestCount;
        if ( gzip.tryInflate( reinterpret_cast<unsigned char*>( buffer.data() ),
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
findDeflateBlocksPragzip( BufferedFileReader::AlignedBuffer buffer )
{
    using DeflateBlock = pragzip::deflate::Block</* CRC32 */ false>;

    const auto nBitsToTest = buffer.size() * CHAR_BIT;
    pragzip::BitReader bitReader( std::make_unique<BufferedFileReader>( std::move( buffer ) ) );

    std::vector<size_t> bitOffsets;

    pragzip::deflate::Block block;
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
            if ( error != pragzip::Error::NONE ) {
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
        } catch ( const pragzip::BitReader::EndOfFileReached& ) {
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
    pragzip::BitReader bitReader( std::make_unique<BufferedFileReader>( std::move( data ) ) );

    uint64_t candidateCount{ 0 };

    using namespace pragzip::blockfinder;
    static const auto nextDeflateCandidateLUT = createNextDeflateCandidateLUT<CACHED_BIT_COUNT>();

    pragzip::deflate::Block block;
    for ( size_t offset = 0; offset <= nBitsToTest; ) {
        bitReader.seek( static_cast<long long int>( offset ) );

        try
        {
            const auto peeked = bitReader.peek<CACHED_BIT_COUNT>();
            const auto nextPosition = nextDeflateCandidateLUT[peeked];  // ~8 MB/s
            //const auto nextPosition = nextDeflateCandidate<CACHED_BIT_COUNT>( peeked );  // ~6 MB/s (slower than LUT!)

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
        } catch ( const pragzip::BitReader::EndOfFileReached& ) {
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
    pragzip::BitReader bitReader( std::make_unique<BufferedFileReader>( std::move( data ) ) );

    uint64_t candidateCount{ 0 };

    using namespace pragzip::blockfinder;
    static const auto nextDeflateCandidateLUT = createNextDeflateCandidateLUT<CACHED_BIT_COUNT>();

    /* For this test, CACHED_BIT_COUNT (<=18) would be sufficient but for the precode check we would need in total
     * 13 + 4 + 57 = 74 bits. We might split this into two buffers of length CACHED_BIT_COUNT and 74 -CACHED_BIT_COUNT
     * because we need the CACHED_BIT_COUNT anyway for much more frequent LUT lookup. */
    auto bitBufferForLUT = bitReader.read<CACHED_BIT_COUNT>();

    pragzip::deflate::Block block;
    try {
        for ( size_t offset = 0; offset <= nBitsToTest; ) {
            auto nextPosition = nextDeflateCandidateLUT[bitBufferForLUT];

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
    } catch ( const pragzip::BitReader::EndOfFileReached& ) {
        /* This might happen when calling readDynamicHuffmanCoding quite some bytes before the end! */
    }

    return candidateCount;
}


enum class CheckPrecodeMethod
{
    WITHOUT_LUT,
    WITHOUT_LUT_USING_ARRAY,
    WALK_TREE_LUT,
    WALK_TREE_COMPRESSED_LUT,
    SINGLE_LUT,
    SINGLE_COMPRESSED_LUT,
};


[[nodiscard]] std::string
toString( CheckPrecodeMethod method )
{
    switch ( method )
    {
    case CheckPrecodeMethod::WITHOUT_LUT             : return "Without LUT";
    case CheckPrecodeMethod::WITHOUT_LUT_USING_ARRAY : return "Without LUT Using Array";
    case CheckPrecodeMethod::WALK_TREE_LUT           : return "Walk Tree LUT";
    case CheckPrecodeMethod::WALK_TREE_COMPRESSED_LUT: return "Walk Tree Compressed LUT";
    case CheckPrecodeMethod::SINGLE_LUT              : return "Single LUT";
    case CheckPrecodeMethod::SINGLE_COMPRESSED_LUT   : return "Single Compressed LUT";
    }
    throw std::invalid_argument( "Unknown check precode method!" );
}


template<CheckPrecodeMethod CHECK_PRECODE_METHOD>
constexpr pragzip::Error
checkPrecode( const uint64_t next4Bits,
              const uint64_t next57Bits )
{
    using namespace pragzip::PrecodeCheck;

    if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::SINGLE_COMPRESSED_LUT ) {
        /**
         * I'm completely baffled that there is no performance gain for this one, which requires
         * only 78 KiB 256 B LUT as opposed to SingleLUT, which requires a 2 MiB LUT.
         * The lookup itself also isn't more expensive because the same bits are now stored in bytes,
         * which avoids a third stage of bit-shifting and masking.
         * @verbatim
         * [13 bits] ( 43.67 <= 44.28 +- 0.26 <= 44.6 ) MB/s
         * [14 bits] ( 44.46 <= 44.69 +- 0.14 <= 44.92 ) MB/s
         * [15 bits] ( 44.07 <= 44.38 +- 0.14 <= 44.61 ) MB/s
         * [16 bits] ( 44.09 <= 44.38 +- 0.2 <= 44.59 ) MB/s
         * [17 bits] ( 44.4 <= 45.2 +- 0.5 <= 45.6 ) MB/s
         * [18 bits] ( 42.6 <= 44.1 +- 0.6 <= 44.5 ) MB/s
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
         * [13 bits] ( 39.1 <= 43.5 +- 1.6 <= 44.5 ) MB/s
         * [14 bits] ( 44.6 <= 45.3 +- 0.4 <= 45.7 ) MB/s
         * [15 bits] ( 43.5 <= 44.2 +- 0.3 <= 44.6 ) MB/s
         * [16 bits] ( 43.2 <= 44.1 +- 0.4 <= 44.6 ) MB/s
         * [17 bits] ( 44.4 <= 45 +- 0.4 <= 45.6 ) MB/s
         * [18 bits] ( 43.09 <= 43.38 +- 0.24 <= 43.78 ) MB/s
         * @endverbatim
         */
        return SingleLUT::checkPrecode( next4Bits, next57Bits );
    }

    if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_COMPRESSED_LUT ) {
         /*
         * @verbatim
         * [13 bits] ( 48.9 <= 49.9 +- 0.6 <= 50.9 ) MB/s
         * [14 bits] ( 49.8 <= 51.2 +- 0.7 <= 52.1 ) MB/s
         * [15 bits] ( 48.5 <= 50.2 +- 0.8 <= 51 ) MB/s
         * [16 bits] ( 46.6 <= 49.4 +- 1.1 <= 50.6 ) MB/s
         * [17 bits] ( 49.7 <= 50.7 +- 0.4 <= 51.1 ) MB/s
         * [18 bits] ( 47.2 <= 47.9 +- 0.4 <= 48.3 ) MB/s
         * @endverbatim
         */
        return WalkTreeCompressedLUT::checkPrecode( next4Bits, next57Bits );
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
         * [13 bits] ( 48.95 <= 49.38 +- 0.28 <= 49.85 ) MB/s
         * [14 bits] ( 49.5 <= 50.7 +- 0.7 <= 51.4 ) MB/s
         * [15 bits] ( 49.9 <= 50.7 +- 0.4 <= 51.2 ) MB/s
         * [16 bits] ( 50.3 <= 50.8 +- 0.3 <= 51.4 ) MB/s
         * [17 bits] ( 49.9 <= 51.2 +- 0.6 <= 52 ) MB/s
         * [18 bits] ( 46.5 <= 49 +- 0.9 <= 49.8 ) MB/s
         * @endverbatim
         */
        return WalkTreeLUT::checkPrecode( next4Bits, next57Bits );
    }

    if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WITHOUT_LUT_USING_ARRAY ) {
        /**
         * @verbatim
         * [13 bits] ( 35.08 <= 35.43 +- 0.21 <= 35.74 ) MB/s
         * [14 bits] ( 33.74 <= 34 +- 0.15 <= 34.21 ) MB/s
         * [15 bits] ( 34.1 <= 34.49 +- 0.23 <= 34.8 ) MB/s
         * [16 bits] ( 33.85 <= 34.09 +- 0.16 <= 34.32 ) MB/s
         * [17 bits] ( 33.3 <= 34 +- 0.4 <= 34.6 ) MB/s
         * [18 bits] ( 34.41 <= 34.89 +- 0.25 <= 35.22 ) MB/s
         * @endverbatim
         */
        return WithoutLUT::checkPrecodeUsingArray( next4Bits, next57Bits );
    }

    if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WITHOUT_LUT ) {
        /**
         * @verbatim
         * [13 bits] ( 33.1 <= 33.28 +- 0.13 <= 33.46 ) MB/s
         * [14 bits] ( 32.93 <= 33.39 +- 0.26 <= 33.79 ) MB/s
         * [15 bits] ( 33.32 <= 33.48 +- 0.12 <= 33.64 ) MB/s
         * [16 bits] ( 32.96 <= 33.2 +- 0.25 <= 33.58 ) MB/s
         * [17 bits] ( 32.9 <= 33.27 +- 0.24 <= 33.61 ) MB/s
         * [18 bits] ( 31.7 <= 32.3 +- 0.3 <= 32.7 ) MB/s
         * @endverbatim
         */
        return WithoutLUT::checkPrecode( next4Bits, next57Bits );
    }

    throw std::invalid_argument( "Unknown check precode method!" );
}


/**
 * Same as findDeflateBlocksPragzip but prefilters calling pragzip using a lookup table and even skips multiple bits.
 * Also, does not find uncompressed blocks nor fixed huffman blocks and as the others no final blocks!
 * The idea is that fixed huffman blocks should be very rare and uncompressed blocks can be found very fast in a
 * separate run over the data (to be implemented).
 */
template<uint8_t            CACHED_BIT_COUNT,
         CheckPrecodeMethod CHECK_PRECODE_METHOD = CheckPrecodeMethod::WALK_TREE_LUT>
[[nodiscard]] std::vector<size_t>
findDeflateBlocksPragzipLUT( BufferedFileReader::AlignedBuffer data )
{
    const size_t nBitsToTest = data.size() * CHAR_BIT;
    pragzip::BitReader bitReader( std::make_unique<BufferedFileReader>( std::move( data ) ) );

    std::vector<size_t> bitOffsets;

    /* Using the constexpr function directly instead of the LUT was found to be slower: ~6 MB/s (vs. 8 MB/s with LUT) */
    static const auto NEXT_DYNAMIC_DEFLATE_CANDIDATE_LUT =
        pragzip::blockfinder::createNextDeflateCandidateLUT<CACHED_BIT_COUNT>();
    const auto oldOffset = bitReader.tell();

    try
    {
        using namespace pragzip::deflate;  /* For the definitions of deflate-specific number of bits. */

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
                       && ( ALL_PRECODE_BITS <= pragzip::BitReader::MAX_BIT_BUFFER_SIZE ),
                       "It must fit into 64-bit and it also must fit the largest possible jump in the LUT." );
        auto bitBufferPrecodeBits = bitReader.read<ALL_PRECODE_BITS>();

        Block block;
        for ( size_t offset = oldOffset; offset <= nBitsToTest; ) {
            auto nextPosition = NEXT_DYNAMIC_DEFLATE_CANDIDATE_LUT[bitBufferForLUT];  // ~8 MB/s

            /* If we can skip forward, then that means that the new position only has been partially checked.
             * Therefore, rechecking the LUT for non-zero skips not only ensures that we aren't wasting time in
             * readHeader but it also ensures that we can avoid checking the first three bits again inside readHeader
             * and instead start reading and checking the dynamic Huffman code directly! */
            if ( nextPosition == 0 ) {
                nextPosition = 1;

                const auto next4Bits = bitBufferPrecodeBits & nLowestBitsSet<uint64_t, PRECODE_COUNT_BITS>();
                const auto next57Bits = ( bitBufferPrecodeBits >> PRECODE_COUNT_BITS )
                                        & nLowestBitsSet<uint64_t, MAX_PRECODE_COUNT * PRECODE_BITS>();
                const auto precodeError = checkPrecode<CHECK_PRECODE_METHOD>( next4Bits, next57Bits );

                if ( UNLIKELY( precodeError == pragzip::Error::NONE ) ) [[unlikely]] {
                #ifndef NDEBUG
                    const auto oldTell = bitReader.tell();
                #endif

                    bitReader.seek( static_cast<long long int>( offset ) + 3 );
                    auto error = block.readDynamicHuffmanCoding( bitReader );
                    if ( UNLIKELY( error == pragzip::Error::NONE ) ) [[unlikely]] {
                        /* Testing decoding is not necessary because the Huffman canonical check is already very strong!
                         * Decoding up to 8 KiB like in pugz only impedes performance and it is harder to reuse that
                         * already decoded data if we do decide that it is a valid block. The number of checks during
                         * reading is also pretty few because there almost are no wasted / invalid symbols. */
                        bitOffsets.push_back( offset );
                    }
                    /* Using this derivable position avoids a possibly costly call to tell() to save the old offet. */
                    bitReader.seek( static_cast<long long int>( offset ) + 13 + ALL_PRECODE_BITS );

                #ifndef NDEBUG
                    if ( oldTell != bitReader.tell() ) {
                        std::cerr << "Previous position: " << oldTell << " new position: " << bitReader.tell() << "\n";
                        throw std::logic_error( "Did not seek back correctly!" );
                    }
                #endif
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
    } catch ( const pragzip::BitReader::EndOfFileReached& ) {
        /* This might happen when calling readDynamicHuffmanCoding quite some bytes before the end! */
    }

    return bitOffsets;
}


[[nodiscard]] std::vector<size_t>
countFilterEfficiencies( BufferedFileReader::AlignedBuffer data )
{
    const size_t nBitsToTest = data.size() * CHAR_BIT;
    pragzip::BitReader bitReader( std::make_unique<BufferedFileReader>( std::move( data ) ) );

    std::vector<size_t> bitOffsets;

    using namespace pragzip::blockfinder;
    static constexpr auto CACHED_BIT_COUNT = 14;
    static const auto nextDeflateCandidateLUT = createNextDeflateCandidateLUT<CACHED_BIT_COUNT>();

    size_t offsetsTestedMoreInDepth{ 0 };
    std::unordered_map<pragzip::Error, uint64_t> errorCounts;
    pragzip::deflate::Block</* CRC32 */ false, /* enable analysis */ true> block;
    size_t checkPrecodeFails{ 0 };
    size_t passedDeflateHeaderTest{ 0 };
    for ( size_t offset = 0; offset <= nBitsToTest; ) {
        bitReader.seek( static_cast<long long int>( offset ) );

        try
        {
            const auto peeked = bitReader.peek<CACHED_BIT_COUNT>();
            const auto nextPosition = nextDeflateCandidateLUT[peeked];

            if ( nextPosition > 0 ) {
                bitReader.seekAfterPeek( nextPosition );
                offset += nextPosition;
                continue;
            }
            ++passedDeflateHeaderTest;

            bitReader.seek( static_cast<long long int>( offset ) + 13 );
            const auto next4Bits = bitReader.read( pragzip::deflate::PRECODE_COUNT_BITS );
            const auto next57Bits = bitReader.peek( pragzip::deflate::MAX_PRECODE_COUNT
                                                    * pragzip::deflate::PRECODE_BITS );
            static_assert( pragzip::deflate::MAX_PRECODE_COUNT * pragzip::deflate::PRECODE_BITS
                           <= pragzip::BitReader::MAX_BIT_BUFFER_SIZE,
                           "This optimization requires a larger BitBuffer inside BitReader!" );
            using pragzip::PrecodeCheck::WalkTreeLUT::checkPrecode;
            if ( checkPrecode( next4Bits, next57Bits ) != pragzip::Error::NONE ) {
                ++checkPrecodeFails;
            }

            offsetsTestedMoreInDepth++;
            bitReader.seek( static_cast<long long int>( offset ) + 3 );
            auto error = block.readDynamicHuffmanCoding( bitReader );

            const auto [count, wasInserted] = errorCounts.try_emplace( error, 1 );
            if ( !wasInserted ) {
                count->second++;
            }

            if ( error != pragzip::Error::NONE ) {
                ++offset;
                continue;
            }

            bitOffsets.push_back( offset );
            ++offset;
        } catch ( const pragzip::BitReader::EndOfFileReached& ) {
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
              << static_cast<double>( block.failedPrecodeInit  ) / static_cast<double>( nBitsToTest ) * 100 << " %)\n"
              << "    Invalid Distance HC: " << block.failedDistanceInit << " ("
              << static_cast<double>( block.failedDistanceInit ) / static_cast<double>( nBitsToTest ) * 100 << " %)\n"
              << "    Invalid Symbol   HC: " << block.failedLengthInit   << " ("
              << static_cast<double>( block.failedLengthInit   ) / static_cast<double>( nBitsToTest ) * 100 << " %)\n"
              << "    Failed checkPrecode calls: " << checkPrecodeFails << "\n\n";

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
    std::multimap<uint64_t, pragzip::Error, std::greater<> > sortedErrorTypes;
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
 * Same as findDeflateBlocksPragzipLUT but tries to improve pipelining by going over the data twice.
 * Once, doing simple Boyer-Moore-like string search tests and skips forward and the second time doing
 * extensive tests by loading and checking the dynamic Huffman trees, which might require seeking back.
 */
template<uint8_t            CACHED_BIT_COUNT,
         CheckPrecodeMethod CHECK_PRECODE_METHOD = CheckPrecodeMethod::WALK_TREE_LUT>
[[nodiscard]] std::vector<size_t>
findDeflateBlocksPragzipLUTTwoPass( BufferedFileReader::AlignedBuffer data )
{
    static_assert( CACHED_BIT_COUNT >= 13,
                   "The LUT must check at least 13-bits, i.e., up to including the distance "
                   "code length check, to avoid duplicate checks in the precode check!" );

    const size_t nBitsToTest = data.size() * CHAR_BIT;
    pragzip::BitReader bitReader( std::make_unique<BufferedFileReader>( std::move( data ) ) );

    std::vector<size_t> bitOffsetCandidates;

    using namespace pragzip::blockfinder;
    static const auto nextDeflateCandidateLUT = createNextDeflateCandidateLUT<CACHED_BIT_COUNT>();

    //const auto t0 = now();
    for ( size_t offset = 0; offset <= nBitsToTest; ) {
        try {
            const auto nextPosition = nextDeflateCandidateLUT[bitReader.peek<CACHED_BIT_COUNT>()];
            if ( nextPosition == 0 ) {
                bitOffsetCandidates.push_back( offset );
                ++offset;
                bitReader.seekAfterPeek( 1 );
            } else {
                offset += nextPosition;
                bitReader.seekAfterPeek( nextPosition );
            }
        } catch ( const pragzip::BitReader::EndOfFileReached& ) {
            break;
        }
    }

    //const auto t1 = now();
    //std::cerr << "    Candidates after first pass: " << bitOffsetCandidates.size()
    //          << ", pass took " << duration( t0, t1 ) << " s\n";

    std::vector<size_t> bitOffsets;

    pragzip::deflate::Block block;

    const auto checkOffset =
        [&] ( const auto offset )
        {
            /* Check the precode Huffman coding. We can skip a lot of the generic tests done in deflate::Block
             * because this is only called for offsets prefiltered by the LUT. But, this also means that the
             * LUT size must be at least 13-bit! */
            try {
                bitReader.seek( static_cast<long long int>( offset ) + 13 );
                const auto next4Bits = bitReader.read( pragzip::deflate::PRECODE_COUNT_BITS );
                const auto next57Bits = bitReader.peek( pragzip::deflate::MAX_PRECODE_COUNT
                                                        * pragzip::deflate::PRECODE_BITS );
                static_assert( pragzip::deflate::MAX_PRECODE_COUNT * pragzip::deflate::PRECODE_BITS
                               <= pragzip::BitReader::MAX_BIT_BUFFER_SIZE,
                               "This optimization requires a larger BitBuffer inside BitReader!" );

                auto error = pragzip::Error::NONE;
                if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::SINGLE_COMPRESSED_LUT ) {
                    error = pragzip::PrecodeCheck::SingleCompressedLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::SINGLE_LUT ) {
                    error = pragzip::PrecodeCheck::SingleLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_LUT ) {
                    error = pragzip::PrecodeCheck::WalkTreeLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_COMPRESSED_LUT ) {
                    error = pragzip::PrecodeCheck::WalkTreeCompressedLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WITHOUT_LUT_USING_ARRAY ) {
                    error = pragzip::PrecodeCheck::WithoutLUT::checkPrecodeUsingArray( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WITHOUT_LUT ) {
                    error = pragzip::PrecodeCheck::WithoutLUT::checkPrecode( next4Bits, next57Bits );
                }

                if ( error != pragzip::Error::NONE ) {
                    return false;
                }
            } catch ( const pragzip::BitReader::EndOfFileReached& ) {}

            try {
                bitReader.seek( static_cast<long long int>( offset ) + 3 );
                return block.readDynamicHuffmanCoding( bitReader ) == pragzip::Error::NONE;
            } catch ( const pragzip::BitReader::EndOfFileReached& ) {}
            return false;
        };

    std::copy_if( bitOffsetCandidates.begin(), bitOffsetCandidates.end(),
                  std::back_inserter( bitOffsets ), checkOffset );

    //std::cerr << "    Candidates after second pass: " << bitOffsets.size()
    //          << ", pass took " << duration( t1 ) << " s\n";

    /**
     * Tested with WalkTreeLUT:
     * Candidates after first pass: 10793212, pass took 0.158863 s
     * Candidates after second pass: 494, pass took 0.318314 s
     */

    return bitOffsets;
}


template<uint8_t            CACHED_BIT_COUNT,
         CheckPrecodeMethod CHECK_PRECODE_METHOD = CheckPrecodeMethod::WALK_TREE_LUT>
[[nodiscard]] std::vector<size_t>
findDeflateBlocksPragzipLUTTwoPassWithPrecode( BufferedFileReader::AlignedBuffer data )
{
    static_assert( CACHED_BIT_COUNT >= 13,
                   "The LUT must check at least 13-bits, i.e., up to including the distance "
                   "code length check, to avoid duplicate checks in the precode check!" );

    const size_t nBitsToTest = data.size() * CHAR_BIT;
    pragzip::BitReader bitReader( std::make_unique<BufferedFileReader>( std::move( data ) ) );

    std::vector<size_t> bitOffsetCandidates;

    using namespace pragzip::blockfinder;
    static const auto NEXT_DYNAMIC_DEFLATE_CANDIDATE_LUT = createNextDeflateCandidateLUT<CACHED_BIT_COUNT>();

    using namespace pragzip::deflate;  /* For the definitions of deflate-specific number of bits. */

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
                   && ( ALL_PRECODE_BITS <= pragzip::BitReader::MAX_BIT_BUFFER_SIZE ),
                   "It must fit into 64-bit and it also must fit the largest possible jump in the LUT." );
    auto bitBufferPrecodeBits = bitReader.read<ALL_PRECODE_BITS>();

    //const auto t0 = now();
    try {
        for ( size_t offset = oldOffset; offset <= nBitsToTest; ) {
            auto nextPosition = NEXT_DYNAMIC_DEFLATE_CANDIDATE_LUT[bitBufferForLUT];
            if ( nextPosition == 0 ) {
                nextPosition = 1;

                const auto next4Bits = bitBufferPrecodeBits & nLowestBitsSet<uint64_t, PRECODE_COUNT_BITS>();
                const auto next57Bits = ( bitBufferPrecodeBits >> PRECODE_COUNT_BITS )
                                        & nLowestBitsSet<uint64_t, MAX_PRECODE_COUNT * PRECODE_BITS>();

                auto precodeError = pragzip::Error::NONE;
                if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::SINGLE_COMPRESSED_LUT ) {
                    precodeError = pragzip::PrecodeCheck::SingleCompressedLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::SINGLE_LUT ) {
                    precodeError = pragzip::PrecodeCheck::SingleLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_LUT ) {
                    precodeError = pragzip::PrecodeCheck::WalkTreeLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WALK_TREE_COMPRESSED_LUT ) {
                    precodeError = pragzip::PrecodeCheck::WalkTreeCompressedLUT::checkPrecode( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WITHOUT_LUT_USING_ARRAY ) {
                    precodeError = pragzip::PrecodeCheck::WithoutLUT::checkPrecodeUsingArray( next4Bits, next57Bits );
                } else if constexpr ( CHECK_PRECODE_METHOD == CheckPrecodeMethod::WITHOUT_LUT ) {
                    precodeError = pragzip::PrecodeCheck::WithoutLUT::checkPrecode( next4Bits, next57Bits );
                }

                if ( UNLIKELY( precodeError == pragzip::Error::NONE ) ) [[unlikely]] {
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
    } catch ( const pragzip::BitReader::EndOfFileReached& ) {
        /* Might happen when testing close to the end. */
    }

    //const auto t1 = now();
    //std::cerr << "    Candidates after first pass: " << bitOffsetCandidates.size()
    //          << ", pass took " << duration( t0, t1 ) << " s\n";

    std::vector<size_t> bitOffsets;

    pragzip::deflate::Block block;

    const auto checkOffset =
        [&] ( const auto offset )
        {
            /* Check the precode Huffman coding. We can skip a lot of the generic tests done in deflate::Block
             * because this is only called for offsets prefiltered by the LUT. But, this also means that the
             * LUT size must be at least 13-bit! */
            try {
                bitReader.seek( static_cast<long long int>( offset ) + 13 );
                const auto next4Bits = bitReader.read( pragzip::deflate::PRECODE_COUNT_BITS );
                const auto next57Bits = bitReader.peek( pragzip::deflate::MAX_PRECODE_COUNT
                                                        * pragzip::deflate::PRECODE_BITS );
                static_assert( pragzip::deflate::MAX_PRECODE_COUNT * pragzip::deflate::PRECODE_BITS
                               <= pragzip::BitReader::MAX_BIT_BUFFER_SIZE,
                               "This optimization requires a larger BitBuffer inside BitReader!" );

                using pragzip::PrecodeCheck::WalkTreeLUT::checkPrecode;
                const auto error = checkPrecode( next4Bits, next57Bits );

                if ( error != pragzip::Error::NONE ) {
                    return false;
                }
            } catch ( const pragzip::BitReader::EndOfFileReached& ) {}

            try {
                bitReader.seek( static_cast<long long int>( offset ) + 3 );
                return block.readDynamicHuffmanCoding( bitReader ) == pragzip::Error::NONE;
            } catch ( const pragzip::BitReader::EndOfFileReached& ) {}
            return false;
        };

    std::copy_if( bitOffsetCandidates.begin(), bitOffsetCandidates.end(),
                  std::back_inserter( bitOffsets ), checkOffset );

    //std::cerr << "    Candidates after second pass: " << bitOffsets.size()
    //          << ", pass took " << duration( t1 ) << " s\n";

    /**
     * Tested with WalkTreeLUT:
     * Candidates after first pass: 43801, pass took 0.167773 s
     * Candidates after second pass: 494, pass took 0.105629 s
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


void
createRandomBase64( const std::string& filePath,
                    const size_t       fileSize )
{
    constexpr std::string_view BASE64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz01234567890+/";
    std::ofstream file{ filePath };
    for ( size_t i = 0; i < fileSize; ++i ) {
        file << ( ( i + 1 == fileSize ) || ( ( i + 1 ) % 77 == 0 )
                  ? '\n' : BASE64[static_cast<size_t>( rand() ) % BASE64.size()] );
    }
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
        std::cout << "    Block candidates (" << blockCandidates.size() << "): "
                  << blockCandidates << "\n\n";
    }

    {
        const auto buffer = bufferFile( fileName, 128_Mi );
        const auto [blockCandidates, durations] = benchmarkFunction<10>(
            [&buffer] () { return findUncompressedDeflateBlocksNestedBranches( buffer ); } );

        std::cout << "[findUncompressedDeflateBlocksNestedBranches] " << formatBandwidth( durations, buffer.size() ) << "\n";
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
        [&blockOffsets = blockOffsets]
        ( const std::vector<size_t>& blockCandidates,
          const size_t               nBytesToTest )
        {
            for ( size_t i = 0; i + 1 < blockOffsets.size(); ++i ) {
                /* Pigz produces a lot of very small fixed Huffman blocks, probably because of a "flush".
                 * But the block finder don't have to find fixed Huffman blocks */
                const auto size = blockOffsets[i + 1] - blockOffsets[i];
                if ( size < 1000 ) {
                    continue;
                }

                /* Especially for the naive zlib finder up to one deflate block might be missing,
                 * i.e., up to ~64 KiB! */
                const auto offset = blockOffsets[i];
                if ( offset >= nBytesToTest * CHAR_BIT - 128_Ki * CHAR_BIT ) {
                    break;
                }

                if ( !contains( blockCandidates, offset ) ) {
                    std::stringstream message;
                    message << "Block " << i << " at offset " << offset << " was not found!";
                    throw std::logic_error( std::move( message ).str() );
                }
            }

            if ( blockCandidates.size() > 2 * blockOffsets.size() + 10 ) {
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
    const auto isBgzfFile = pragzip::blockfinder::Bgzf::isBgzfFile( std::make_unique<StandardFileReader>( fileName ) );
    if ( !isBgzfFile ) {
        const auto buffer = bufferFile( fileName, 256_Ki );
        const auto [blockCandidates, durations] = benchmarkFunction<10>(
            [&buffer] () { return findDeflateBlocksZlibOptimized( buffer ); } );

        std::cout << "[findDeflateBlocksZlibOptimized] " << formatBandwidth( durations, buffer.size() ) << "\n";
        std::cout << "    Block candidates (" << blockCandidates.size() << "): "
                  << blockCandidates << "\n\n";
    }

    /* Benchmarks with own implementation (pragzip). */
    {
        static constexpr auto OPTIMAL_NEXT_DEFLATE_LUT_SIZE = pragzip::blockfinder::OPTIMAL_NEXT_DEFLATE_LUT_SIZE;
        const auto buffer = bufferFile( fileName, 16_Mi );

        const auto blockCandidates = countFilterEfficiencies( buffer );
        std::cout << "Block candidates (" << blockCandidates.size() << "): " << blockCandidates << "\n\n";

        const auto [blockCandidatesPragzip, durations] = benchmarkFunction<10>(
            [&buffer] () { return findDeflateBlocksPragzip( buffer ); } );

        if ( blockCandidates != blockCandidatesPragzip ) {
            std::stringstream msg;
            msg << "Results with findDeflateBlocksPragzip differ! Block candidates ("
                << blockCandidatesPragzip.size() << "): " << blockCandidatesPragzip;
            throw std::logic_error( std::move( msg ).str() );
        }
        std::cout << std::setw( 37 ) << std::left << "[findDeflateBlocksPragzip] " << std::right
                  << formatBandwidth( durations, buffer.size() ) << "\n";

        /* Same as above but with a LUT that can skip bits similar to the Boyer–Moore string-search algorithm. */

        /* Call findDeflateBlocksPragzipLUT once to initialize the static variables! */
        if ( const auto blockCandidatesLUT = findDeflateBlocksPragzipLUT<OPTIMAL_NEXT_DEFLATE_LUT_SIZE>( buffer );
             blockCandidatesLUT != blockCandidates ) {
            std::stringstream msg;
            msg << "Results with findDeflateBlocksPragzipLUT differ! Block candidates ("
                << blockCandidatesLUT.size() << "): " << blockCandidatesLUT;
            throw std::logic_error( std::move( msg ).str() );
        }

        const auto [blockCandidatesLUT, durationsLUT] = benchmarkFunction<10>(
            /* As for choosing CACHED_BIT_COUNT == 13, see the output of the results at the end of the file.
             * 13 is the last for which it significantly improves over less bits and 14 bits produce reproducibly
             * slower bandwidths! 13 bits is the best configuration as far as I know. */
            [&buffer] () { return findDeflateBlocksPragzipLUT<OPTIMAL_NEXT_DEFLATE_LUT_SIZE>( buffer ); } );

        if ( blockCandidates != blockCandidatesLUT ) {
            std::stringstream msg;
            msg << "Results with findDeflateBlocksPragzipLUT differ! Block candidates ("
                << blockCandidatesLUT.size() << "): " << blockCandidatesLUT;
            throw std::logic_error( std::move( msg ).str() );
        }
        std::cout << std::setw( 37 ) << std::left << "[findDeflateBlocksPragzipLUT] " << std::right
                  << formatBandwidth( durationsLUT, buffer.size() ) << "\n";

        /* Same as above but with a LUT and two-pass. */

        const auto [blockCandidatesLUT2P, durationsLUT2P] = benchmarkFunction<10>(
            /* As for choosing CACHED_BIT_COUNT == 13, see the output of the results at the end of the file.
             * 13 is the last for which it significantly improves over less bits and 14 bits produce reproducibly
             * slower bandwidths! 13 bits is the best configuration as far as I know. */
            [&buffer] () { return findDeflateBlocksPragzipLUTTwoPass<OPTIMAL_NEXT_DEFLATE_LUT_SIZE>( buffer ); } );

        if ( blockCandidates != blockCandidatesLUT2P ) {
            std::stringstream msg;
            msg << "Results with findDeflateBlocksPragzipLUTTwoPass differ! Block candidates ("
                << blockCandidatesLUT2P.size() << "): " << blockCandidatesLUT2P;
            throw std::logic_error( std::move( msg ).str() );
        }
        std::cout << "[findDeflateBlocksPragzipLUTTwoPass] " << formatBandwidth( durationsLUT2P, buffer.size() ) << "\n";
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

    std::cout << "[findDeflateBlocksPragzipLUT with " << static_cast<int>( CACHED_BIT_COUNT ) << " bits] "
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

    std::cout << "[findDeflateBlocksPragzipLUT with " << static_cast<int>( CACHED_BIT_COUNT ) << " bits] "
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
    case FindDeflateMethod::FULL_CHECK            : return "findDeflateBlocksPragzipLUT";
    case FindDeflateMethod::TWO_PASS              : return "findDeflateBlocksPragzipLUTTwoPass";
    case FindDeflateMethod::TWO_PASS_WITH_PRECODE : return "findDeflateBlocksPragzipLUTTwoPassAndPrecode";
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
                return findDeflateBlocksPragzipLUT<CACHED_BIT_COUNT, CHECK_PRECODE_METHOD>( buffer );
            } else if constexpr ( FIND_DEFLATE_METHOD == FindDeflateMethod::TWO_PASS ) {
                return findDeflateBlocksPragzipLUTTwoPass<CACHED_BIT_COUNT, CHECK_PRECODE_METHOD>( buffer );
            } else if constexpr ( FIND_DEFLATE_METHOD == FindDeflateMethod::TWO_PASS_WITH_PRECODE ) {
                return findDeflateBlocksPragzipLUTTwoPassWithPrecode<CACHED_BIT_COUNT, CHECK_PRECODE_METHOD>( buffer );
            }
        } );

    std::cout << "[" << toString( FIND_DEFLATE_METHOD ) << " with " << static_cast<int>( CACHED_BIT_COUNT ) << " bits, "
              << toString( CHECK_PRECODE_METHOD ) << "] "
              << formatBandwidth( durations, buffer.size() ) << "\n";

    if ( blockCandidatesWithLessBits && ( *blockCandidatesWithLessBits != blockCandidates ) ) {
        std::cerr << "blockCandidatesWithLessBits (" << blockCandidatesWithLessBits->size() << "):"
                  << *blockCandidatesWithLessBits << "\n"
                  << "blockCandidates (" << blockCandidates.size() << "):" << blockCandidates << "\n";
        throw std::logic_error( "Got inconsistent block candidates for pragzip blockfinder with different "
                                "LUT table sizes!" );
    }

    return blockCandidates;
}


template<uint8_t MIN_CACHED_BIT_COUNT,
         uint8_t MAX_CACHED_BIT_COUNT,
         uint8_t CACHED_BIT_COUNT = MIN_CACHED_BIT_COUNT>
void
analyzeDeflateJumpLUT()
{
    using namespace pragzip::blockfinder;
    static const auto LUT = createNextDeflateCandidateLUT<CACHED_BIT_COUNT>();

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


int
main( int    argc,
      char** argv )
{
    if ( argc > 1 ) {
        for ( int i = 1; i < argc; ++i ) {
            if ( std::filesystem::exists( argv[i] ) ) {
                benchmarkGzip( argv[i] );
            }
        }
    }

    const auto tmpFolder = createTemporaryDirectory( "pragzip.benchmarkGzipBlockFinder" );
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


            /* Benchmark Pragzip LUT version with different LUT sizes. */

            if ( name == "gzip" ) {
                const auto data = bufferFile( newFileName );

                /* CACHED_BIT_COUNT == 19 fails on GCC because it requires > 99 M constexpr steps.
                 * CACHED_BIT_COUNT == 18 fail on clang because it requires > 99 M constexpr steps.
                 * It works when using simple const instead of constexpr.
                 * This is a maximum cached bit count. It will benchmark all the way down to 13. */
                constexpr auto CACHED_BIT_COUNT = 18U;

            /* Do not always compile and run all tests because it increases compile-time and runtime a lot. */
            //#define BENCHMARK_ALL_VERSIONS_WITH_DIFFERENT_JUMP_LUT_SIZES
            #ifdef BENCHMARK_ALL_VERSIONS_WITH_DIFFERENT_JUMP_LUT_SIZES

                std::cout << "== Testing different pragzip deflate header jump LUT table sizes ==\n\n";
                std::cout << "=== Only using the skip LUT (many false positives) and manual sliding bit buffer ===\n\n";
                const auto candidateCountManualSkipping =
                    benchmarkLUTSizeOnlySkipManualSlidingBufferLUT<CACHED_BIT_COUNT>( data );
                std::cout << "\n\n";

                std::cout << "=== Only using the skip LUT (many false positives) ===\n\n";
                const auto candidateCountSkipLUTOnly = benchmarkLUTSizeOnlySkipLUT<CACHED_BIT_COUNT>( data );
                std::cout << "\n\n";

                REQUIRE_EQUAL( candidateCountManualSkipping, candidateCountSkipLUTOnly );

                std::cout << "=== Full test and precode check ===\n\n";
                constexpr auto FULL_CHECK = FindDeflateMethod::FULL_CHECK;
                benchmarkLUTSize<CACHED_BIT_COUNT, FULL_CHECK, CheckPrecodeMethod::WITHOUT_LUT>( data );
                std::cout << "\n";
                benchmarkLUTSize<CACHED_BIT_COUNT, FULL_CHECK, CheckPrecodeMethod::WITHOUT_LUT_USING_ARRAY>( data );
                std::cout << "\n";
                benchmarkLUTSize<CACHED_BIT_COUNT, FULL_CHECK, CheckPrecodeMethod::WALK_TREE_LUT>( data );
                std::cout << "\n";
                benchmarkLUTSize<CACHED_BIT_COUNT, FULL_CHECK, CheckPrecodeMethod::WALK_TREE_COMPRESSED_LUT>( data );
                std::cout << "\n";
                benchmarkLUTSize<CACHED_BIT_COUNT, FULL_CHECK, CheckPrecodeMethod::SINGLE_LUT>( data );
                std::cout << "\n";
                benchmarkLUTSize<CACHED_BIT_COUNT, FULL_CHECK, CheckPrecodeMethod::SINGLE_COMPRESSED_LUT>( data );
                std::cout << "\n\n";

                std::cout << "=== Full test and precode check in two passes ===\n\n";
                constexpr auto TWO_PASS = FindDeflateMethod::TWO_PASS;
                benchmarkLUTSize<CACHED_BIT_COUNT, TWO_PASS, CheckPrecodeMethod::WITHOUT_LUT>( data );
                std::cout << "\n";
                benchmarkLUTSize<CACHED_BIT_COUNT, TWO_PASS, CheckPrecodeMethod::WITHOUT_LUT_USING_ARRAY>( data );
                std::cout << "\n";
                benchmarkLUTSize<CACHED_BIT_COUNT, TWO_PASS, CheckPrecodeMethod::WALK_TREE_LUT>( data );
                std::cout << "\n";
                benchmarkLUTSize<CACHED_BIT_COUNT, TWO_PASS, CheckPrecodeMethod::WALK_TREE_COMPRESSED_LUT>( data );
                std::cout << "\n";
                benchmarkLUTSize<CACHED_BIT_COUNT, TWO_PASS, CheckPrecodeMethod::SINGLE_LUT>( data );
                std::cout << "\n";
                benchmarkLUTSize<CACHED_BIT_COUNT, TWO_PASS, CheckPrecodeMethod::SINGLE_COMPRESSED_LUT>( data );
                std::cout << "\n\n";

                std::cout << "=== Full test and precode check in two passes and precode check in first pass ===\n\n";
                constexpr auto TWO_PASS_PRE = FindDeflateMethod::TWO_PASS_WITH_PRECODE;
                benchmarkLUTSize<CACHED_BIT_COUNT, TWO_PASS_PRE, CheckPrecodeMethod::WITHOUT_LUT>( data );
                std::cout << "\n";
                benchmarkLUTSize<CACHED_BIT_COUNT, TWO_PASS_PRE, CheckPrecodeMethod::WITHOUT_LUT_USING_ARRAY>( data );
                std::cout << "\n";
                benchmarkLUTSize<CACHED_BIT_COUNT, TWO_PASS_PRE, CheckPrecodeMethod::WALK_TREE_LUT>( data );
                std::cout << "\n";
                benchmarkLUTSize<CACHED_BIT_COUNT, TWO_PASS_PRE, CheckPrecodeMethod::WALK_TREE_COMPRESSED_LUT>( data );
                std::cout << "\n";
                benchmarkLUTSize<CACHED_BIT_COUNT, TWO_PASS_PRE, CheckPrecodeMethod::SINGLE_LUT>( data );
                std::cout << "\n";
                benchmarkLUTSize<CACHED_BIT_COUNT, TWO_PASS_PRE, CheckPrecodeMethod::SINGLE_COMPRESSED_LUT>( data );
                std::cout << "\n\n";
            #else
                std::cout << "=== Full test and precode check ===\n\n";
                benchmarkLUTSize<CACHED_BIT_COUNT,
                                 FindDeflateMethod::FULL_CHECK,
                                 CheckPrecodeMethod::WALK_TREE_COMPRESSED_LUT>( data );
                benchmarkLUTSize<CACHED_BIT_COUNT,
                                 FindDeflateMethod::FULL_CHECK,
                                 CheckPrecodeMethod::WALK_TREE_LUT>( data );
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
( set -o pipefail; cmake --build . -- benchmarkGzipBlockFinder 2>&1 | stdbuf -o0 tee build.log ) &&
stdbuf -o0 -e0 taskset 0x08 src/benchmarks/benchmarkGzipBlockFinder | stdbuf -o0 tee benchmark.log

Deflate Jump LUT for 13 bits is sized: 8 KiB with the following jump distance distribution:
     0 :   900 (10.9863 %)
     1 :   960 (11.7188 %)
     2 :   960 (11.7188 %)
     3 :   846 (10.3271 %)
     4 :   724 (8.83789 %)
     5 :   600 (7.32422 %)
     6 :   535 (6.53076 %)
     7 :   444 (5.41992 %)
     8 :   360 (4.39453 %)
     9 :   302 (3.68652 %)
    10 :   256 (3.125 %)
    11 :   436 (5.32227 %)
    12 :   309 (3.77197 %)
    13 :   560 (6.83594 %)

Deflate Jump LUT for 14 bits is sized: 16 KiB with the following jump distance distribution:
     0 :  1800 (10.9863 %)
     1 :  1800 (10.9863 %)
     2 :  1920 (11.7188 %)
     3 :  1692 (10.3271 %)
     4 :  1460 (8.91113 %)
     5 :  1208 (7.37305 %)
     6 :   997 (6.08521 %)
     7 :   902 (5.50537 %)
     8 :   744 (4.54102 %)
     9 :   604 (3.68652 %)
    10 :   512 (3.125 %)
    11 :   436 (2.66113 %)
    12 :   745 (4.54712 %)
    13 :   560 (3.41797 %)
    14 :  1004 (6.12793 %)

Deflate Jump LUT for 15 bits is sized: 32 KiB with the following jump distance distribution:
     0 :  3600 (10.9863 %)
     1 :  3600 (10.9863 %)
     2 :  3600 (10.9863 %)
     3 :  3384 (10.3271 %)
     4 :  2920 (8.91113 %)
     5 :  2440 (7.44629 %)
     6 :  2010 (6.13403 %)
     7 :  1670 (5.09644 %)
     8 :  1516 (4.62646 %)
     9 :  1256 (3.83301 %)
    10 :  1024 (3.125 %)
    11 :   872 (2.66113 %)
    12 :   745 (2.27356 %)
    13 :  1305 (3.98254 %)
    14 :  1004 (3.06396 %)
    15 :  1822 (5.5603 %)

Deflate Jump LUT for 16 bits is sized: 64 KiB with the following jump distance distribution:
     0 :  7200 (10.9863 %)
     1 :  7200 (10.9863 %)
     2 :  7200 (10.9863 %)
     3 :  6342 (9.67712 %)
     4 :  5840 (8.91113 %)
     5 :  4880 (7.44629 %)
     6 :  4062 (6.19812 %)
     7 :  3368 (5.13916 %)
     8 :  2800 (4.27246 %)
     9 :  2561 (3.90778 %)
    10 :  2132 (3.25317 %)
    11 :  1744 (2.66113 %)
    12 :  1490 (2.27356 %)
    13 :  1305 (1.99127 %)
    14 :  2309 (3.52325 %)
    15 :  1822 (2.78015 %)
    16 :  3281 (5.00641 %)

Deflate Jump LUT for 17 bits is sized: 128 KiB with the following jump distance distribution:
     0 : 14400 (10.9863 %)
     1 : 14400 (10.9863 %)
     2 : 14400 (10.9863 %)
     3 : 12684 (9.67712 %)
     4 : 10944 (8.34961 %)
     5 :  9760 (7.44629 %)
     6 :  8124 (6.19812 %)
     7 :  6808 (5.19409 %)
     8 :  5648 (4.30908 %)
     9 :  4723 (3.60336 %)
    10 :  4348 (3.31726 %)
    11 :  3632 (2.771 %)
    12 :  2980 (2.27356 %)
    13 :  2610 (1.99127 %)
    14 :  2309 (1.76163 %)
    15 :  4131 (3.1517 %)
    16 :  3281 (2.5032 %)
    17 :  5890 (4.49371 %)

Deflate Jump LUT for 18 bits is sized: 256 KiB with the following jump distance distribution:
     0 : 28800 (10.9863 %)
     1 : 28800 (10.9863 %)
     2 : 28800 (10.9863 %)
     3 : 25368 (9.67712 %)
     4 : 21888 (8.34961 %)
     5 : 18288 (6.97632 %)
     6 : 16248 (6.19812 %)
     7 : 13616 (5.19409 %)
     8 : 11416 (4.35486 %)
     9 :  9526 (3.63388 %)
    10 :  8016 (3.05786 %)
    11 :  7404 (2.8244 %)
    12 :  6200 (2.36511 %)
    13 :  5220 (1.99127 %)
    14 :  4618 (1.76163 %)
    15 :  4131 (1.57585 %)
    16 :  7412 (2.82745 %)
    17 :  5890 (2.24686 %)
    18 : 10503 (4.00658 %)


== Testing different pragzip deflate header jump LUT table sizes ==

=== Only using the skip LUT (many false positives) and manual sliding bit buffer ===

[findDeflateBlocksPragzipLUT with 13 bits] ( 121 <= 139 +- 8 <= 150 ) MB/s (10793215 candidates)
[findDeflateBlocksPragzipLUT with 14 bits] ( 154.8 <= 157.1 +- 1.7 <= 159.4 ) MB/s (10793215 candidates)
[findDeflateBlocksPragzipLUT with 15 bits] ( 143 <= 161 +- 7 <= 166 ) MB/s (10793215 candidates)
[findDeflateBlocksPragzipLUT with 16 bits] ( 136 <= 144 +- 4 <= 147 ) MB/s (10793215 candidates)
[findDeflateBlocksPragzipLUT with 17 bits] ( 133.7 <= 138 +- 2.4 <= 141.3 ) MB/s (10793215 candidates)
[findDeflateBlocksPragzipLUT with 18 bits] ( 119 <= 130 +- 5 <= 135 ) MB/s (10793215 candidates)


=== Only using the skip LUT (many false positives) ===

[findDeflateBlocksPragzipLUT with 13 bits] ( 44.9 <= 47.2 +- 1 <= 48.1 ) MB/s (10793215 candidates)
[findDeflateBlocksPragzipLUT with 14 bits] ( 50.9 <= 52.1 +- 0.6 <= 52.7 ) MB/s (10793215 candidates)
[findDeflateBlocksPragzipLUT with 15 bits] ( 53.19 <= 53.69 +- 0.27 <= 54 ) MB/s (10793215 candidates)
[findDeflateBlocksPragzipLUT with 16 bits] ( 50.4 <= 52.6 +- 1.1 <= 53.8 ) MB/s (10793215 candidates)
[findDeflateBlocksPragzipLUT with 17 bits] ( 52.1 <= 53.8 +- 0.7 <= 54.4 ) MB/s (10793215 candidates)
[findDeflateBlocksPragzipLUT with 18 bits] ( 49.7 <= 52 +- 1.1 <= 52.8 ) MB/s (10793215 candidates)


=== Full test and precode check ===

[findDeflateBlocksPragzipLUT with 13 bits, Without LUT] ( 32.65 <= 33.1 +- 0.27 <= 33.58 ) MB/s
[findDeflateBlocksPragzipLUT with 14 bits, Without LUT] ( 32.54 <= 33.1 +- 0.22 <= 33.3 ) MB/s
[findDeflateBlocksPragzipLUT with 15 bits, Without LUT] ( 32.3 <= 33.2 +- 0.4 <= 33.6 ) MB/s
[findDeflateBlocksPragzipLUT with 16 bits, Without LUT] ( 32.2 <= 32.9 +- 0.3 <= 33.2 ) MB/s
[findDeflateBlocksPragzipLUT with 17 bits, Without LUT] ( 32.61 <= 33.16 +- 0.21 <= 33.38 ) MB/s
[findDeflateBlocksPragzipLUT with 18 bits, Without LUT] ( 32.1 <= 32.5 +- 0.17 <= 32.68 ) MB/s

[findDeflateBlocksPragzipLUT with 13 bits, Without LUT Using Array] ( 33.71 <= 34.1 +- 0.21 <= 34.41 ) MB/s
[findDeflateBlocksPragzipLUT with 14 bits, Without LUT Using Array] ( 33.54 <= 33.99 +- 0.22 <= 34.28 ) MB/s
[findDeflateBlocksPragzipLUT with 15 bits, Without LUT Using Array] ( 33.78 <= 34.16 +- 0.21 <= 34.36 ) MB/s
[findDeflateBlocksPragzipLUT with 16 bits, Without LUT Using Array] ( 34.94 <= 35.28 +- 0.19 <= 35.6 ) MB/s
[findDeflateBlocksPragzipLUT with 17 bits, Without LUT Using Array] ( 35.12 <= 35.36 +- 0.15 <= 35.59 ) MB/s
[findDeflateBlocksPragzipLUT with 18 bits, Without LUT Using Array] ( 32.05 <= 32.54 +- 0.25 <= 32.77 ) MB/s

[findDeflateBlocksPragzipLUT with 13 bits, Walk Tree LUT] ( 49 <= 49.7 +- 0.4 <= 50.4 ) MB/s
[findDeflateBlocksPragzipLUT with 14 bits, Walk Tree LUT] ( 48.3 <= 49.5 +- 0.7 <= 50.6 ) MB/s
[findDeflateBlocksPragzipLUT with 15 bits, Walk Tree LUT] ( 46.9 <= 48.4 +- 1.2 <= 49.9 ) MB/s
[findDeflateBlocksPragzipLUT with 16 bits, Walk Tree LUT] ( 47.8 <= 49.1 +- 0.7 <= 50.4 ) MB/s
[findDeflateBlocksPragzipLUT with 17 bits, Walk Tree LUT] ( 49.1 <= 50 +- 0.4 <= 50.4 ) MB/s
[findDeflateBlocksPragzipLUT with 18 bits, Walk Tree LUT] ( 45.8 <= 47.6 +- 0.7 <= 48.1 ) MB/s

[findDeflateBlocksPragzipLUT with 13 bits, Walk Tree Compressed LUT] ( 47.6 <= 49.3 +- 0.7 <= 50 ) MB/s
[findDeflateBlocksPragzipLUT with 14 bits, Walk Tree Compressed LUT] ( 47.3 <= 49.5 +- 0.8 <= 50.1 ) MB/s
[findDeflateBlocksPragzipLUT with 15 bits, Walk Tree Compressed LUT] ( 46.8 <= 49 +- 1.1 <= 50.1 ) MB/s
[findDeflateBlocksPragzipLUT with 16 bits, Walk Tree Compressed LUT] ( 47 <= 48.6 +- 0.8 <= 49.8 ) MB/s
[findDeflateBlocksPragzipLUT with 17 bits, Walk Tree Compressed LUT] ( 46.8 <= 48.7 +- 1.1 <= 49.7 ) MB/s
[findDeflateBlocksPragzipLUT with 18 bits, Walk Tree Compressed LUT] ( 47.7 <= 48.4 +- 0.5 <= 49.1 ) MB/s

[findDeflateBlocksPragzipLUT with 13 bits, Single LUT] ( 43.4 <= 44.2 +- 0.4 <= 44.6 ) MB/s
[findDeflateBlocksPragzipLUT with 14 bits, Single LUT] ( 43.7 <= 44.3 +- 0.5 <= 45.1 ) MB/s
[findDeflateBlocksPragzipLUT with 15 bits, Single LUT] ( 42.8 <= 43.9 +- 0.7 <= 44.7 ) MB/s
[findDeflateBlocksPragzipLUT with 16 bits, Single LUT] ( 41.6 <= 43 +- 0.7 <= 43.8 ) MB/s
[findDeflateBlocksPragzipLUT with 17 bits, Single LUT] ( 44.1 <= 44.6 +- 0.3 <= 45 ) MB/s
[findDeflateBlocksPragzipLUT with 18 bits, Single LUT] ( 44.03 <= 44.46 +- 0.24 <= 44.82 ) MB/s

[findDeflateBlocksPragzipLUT with 13 bits, Single Compressed LUT] ( 44.2 <= 44.6 +- 0.3 <= 45.1 ) MB/s
[findDeflateBlocksPragzipLUT with 14 bits, Single Compressed LUT] ( 43.7 <= 44.5 +- 0.3 <= 44.9 ) MB/s
[findDeflateBlocksPragzipLUT with 15 bits, Single Compressed LUT] ( 43.72 <= 44.03 +- 0.2 <= 44.52 ) MB/s
[findDeflateBlocksPragzipLUT with 16 bits, Single Compressed LUT] ( 43.7 <= 44.4 +- 0.3 <= 44.8 ) MB/s
[findDeflateBlocksPragzipLUT with 17 bits, Single Compressed LUT] ( 41.5 <= 44.4 +- 1 <= 45.1 ) MB/s
[findDeflateBlocksPragzipLUT with 18 bits, Single Compressed LUT] ( 43.3 <= 43.8 +- 0.3 <= 44.1 ) MB/s


=== Full test and precode check in two passes ===

[findDeflateBlocksPragzipLUTTwoPass with 13 bits, Without LUT] ( 20.05 <= 20.24 +- 0.13 <= 20.47 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 14 bits, Without LUT] ( 19.81 <= 20.36 +- 0.23 <= 20.59 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 15 bits, Without LUT] ( 20.33 <= 20.43 +- 0.08 <= 20.56 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 16 bits, Without LUT] ( 20.02 <= 20.2 +- 0.13 <= 20.4 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 17 bits, Without LUT] ( 19.88 <= 20.06 +- 0.15 <= 20.33 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 18 bits, Without LUT] ( 19.69 <= 20.05 +- 0.14 <= 20.16 ) MB/s

[findDeflateBlocksPragzipLUTTwoPass with 13 bits, Without LUT Using Array] ( 20.5 <= 20.59 +- 0.06 <= 20.67 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 14 bits, Without LUT Using Array] ( 20.19 <= 20.37 +- 0.1 <= 20.52 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 15 bits, Without LUT Using Array] ( 20.76 <= 20.89 +- 0.07 <= 20.96 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 16 bits, Without LUT Using Array] ( 19.76 <= 19.95 +- 0.13 <= 20.13 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 17 bits, Without LUT Using Array] ( 19.65 <= 19.84 +- 0.1 <= 20.01 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 18 bits, Without LUT Using Array] ( 19.56 <= 19.71 +- 0.1 <= 19.86 ) MB/s

[findDeflateBlocksPragzipLUTTwoPass with 13 bits, Walk Tree LUT] ( 25.59 <= 25.91 +- 0.15 <= 26.1 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 14 bits, Walk Tree LUT] ( 25.52 <= 25.94 +- 0.18 <= 26.12 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 15 bits, Walk Tree LUT] ( 25.39 <= 26 +- 0.26 <= 26.24 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 16 bits, Walk Tree LUT] ( 25.43 <= 25.63 +- 0.13 <= 25.82 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 17 bits, Walk Tree LUT] ( 25.24 <= 25.62 +- 0.18 <= 25.9 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 18 bits, Walk Tree LUT] ( 24.1 <= 25.1 +- 0.4 <= 25.4 ) MB/s

[findDeflateBlocksPragzipLUTTwoPass with 13 bits, Walk Tree Compressed LUT] ( 25.83 <= 26.04 +- 0.13 <= 26.26 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 14 bits, Walk Tree Compressed LUT] ( 25.89 <= 26.21 +- 0.17 <= 26.54 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 15 bits, Walk Tree Compressed LUT] ( 25.55 <= 25.96 +- 0.18 <= 26.2 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 16 bits, Walk Tree Compressed LUT] ( 25.76 <= 25.89 +- 0.08 <= 26.02 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 17 bits, Walk Tree Compressed LUT] ( 25.42 <= 25.65 +- 0.12 <= 25.78 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 18 bits, Walk Tree Compressed LUT] ( 24.62 <= 24.91 +- 0.12 <= 25.04 ) MB/s

[findDeflateBlocksPragzipLUTTwoPass with 13 bits, Single LUT] ( 24.21 <= 24.34 +- 0.09 <= 24.46 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 14 bits, Single LUT] ( 24.26 <= 24.5 +- 0.11 <= 24.67 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 15 bits, Single LUT] ( 23.92 <= 24.18 +- 0.16 <= 24.45 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 16 bits, Single LUT] ( 23.9 <= 24.18 +- 0.14 <= 24.31 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 17 bits, Single LUT] ( 23.71 <= 23.86 +- 0.08 <= 23.97 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 18 bits, Single LUT] ( 23.65 <= 23.79 +- 0.09 <= 23.91 ) MB/s

[findDeflateBlocksPragzipLUTTwoPass with 13 bits, Single Compressed LUT] ( 23.44 <= 24.21 +- 0.28 <= 24.41 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 14 bits, Single Compressed LUT] ( 24.31 <= 24.68 +- 0.16 <= 24.87 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 15 bits, Single Compressed LUT] ( 23.1 <= 24.1 +- 0.6 <= 24.6 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 16 bits, Single Compressed LUT] ( 21.5 <= 23 +- 0.6 <= 23.6 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 17 bits, Single Compressed LUT] ( 22.77 <= 23.19 +- 0.24 <= 23.48 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass with 18 bits, Single Compressed LUT] ( 23.02 <= 23.32 +- 0.17 <= 23.49 ) MB/s


=== Full test and precode check in two passes and precode check in first pass ===

[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 13 bits, Without LUT] ( 32.2 <= 32.49 +- 0.13 <= 32.62 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 14 bits, Without LUT] ( 33.32 <= 33.49 +- 0.15 <= 33.83 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 15 bits, Without LUT] ( 32.4 <= 32.7 +- 0.17 <= 32.87 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 16 bits, Without LUT] ( 32.23 <= 32.56 +- 0.19 <= 32.78 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 17 bits, Without LUT] ( 31.82 <= 32.15 +- 0.21 <= 32.44 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 18 bits, Without LUT] ( 31.3 <= 32 +- 0.4 <= 32.5 ) MB/s

[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 13 bits, Without LUT Using Array] ( 32.2 <= 33.1 +- 0.4 <= 33.5 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 14 bits, Without LUT Using Array] ( 33.3 <= 34.2 +- 0.4 <= 34.7 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 15 bits, Without LUT Using Array] ( 33.4 <= 34 +- 0.3 <= 34.4 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 16 bits, Without LUT Using Array] ( 32.87 <= 33.3 +- 0.24 <= 33.53 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 17 bits, Without LUT Using Array] ( 32.82 <= 33.28 +- 0.23 <= 33.6 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 18 bits, Without LUT Using Array] ( 32.7 <= 33.1 +- 0.3 <= 33.6 ) MB/s

[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 13 bits, Walk Tree LUT] ( 36 <= 46 +- 5 <= 50 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 14 bits, Walk Tree LUT] ( 49.84 <= 50.21 +- 0.19 <= 50.46 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 15 bits, Walk Tree LUT] ( 49.05 <= 49.33 +- 0.2 <= 49.72 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 16 bits, Walk Tree LUT] ( 49.39 <= 49.67 +- 0.14 <= 49.91 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 17 bits, Walk Tree LUT] ( 49.1 <= 49.8 +- 0.4 <= 50.2 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 18 bits, Walk Tree LUT] ( 47.7 <= 48.6 +- 0.4 <= 49.2 ) MB/s

[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 13 bits, Walk Tree Compressed LUT] ( 47.9 <= 48.6 +- 0.3 <= 49.1 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 14 bits, Walk Tree Compressed LUT] ( 48.3 <= 49.1 +- 0.5 <= 49.6 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 15 bits, Walk Tree Compressed LUT] ( 47 <= 47.9 +- 0.5 <= 48.7 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 16 bits, Walk Tree Compressed LUT] ( 48.1 <= 48.6 +- 0.4 <= 49.3 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 17 bits, Walk Tree Compressed LUT] ( 47.9 <= 48.9 +- 0.8 <= 49.8 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 18 bits, Walk Tree Compressed LUT] ( 47.8 <= 48.7 +- 0.5 <= 49.2 ) MB/s

[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 13 bits, Single LUT] ( 43.21 <= 43.61 +- 0.2 <= 43.81 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 14 bits, Single LUT] ( 43.4 <= 43.8 +- 0.4 <= 44.4 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 15 bits, Single LUT] ( 42.3 <= 43.2 +- 0.4 <= 43.9 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 16 bits, Single LUT] ( 43.4 <= 43.8 +- 0.3 <= 44.3 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 17 bits, Single LUT] ( 42.2 <= 43.3 +- 0.8 <= 44.4 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 18 bits, Single LUT] ( 41.6 <= 43.2 +- 0.6 <= 43.7 ) MB/s

[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 13 bits, Single Compressed LUT] ( 41.8 <= 43.1 +- 0.7 <= 43.8 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 14 bits, Single Compressed LUT] ( 42.6 <= 44.1 +- 0.7 <= 44.7 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 15 bits, Single Compressed LUT] ( 41.8 <= 43 +- 0.6 <= 43.6 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 16 bits, Single Compressed LUT] ( 43.25 <= 43.6 +- 0.21 <= 43.85 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 17 bits, Single Compressed LUT] ( 42.6 <= 43.7 +- 0.6 <= 44.7 ) MB/s
[findDeflateBlocksPragzipLUTTwoPassAndPrecode with 18 bits, Single Compressed LUT] ( 43.31 <= 43.62 +- 0.18 <= 43.9 ) MB/s


=== Testing with encoder: gzip ===

> gzip --version
gzip 1.10
Copyright (C) 2018 Free Software Foundation, Inc.
Copyright (C) 1993 Jean-loup Gailly.
This is free software.  You may redistribute copies of it under the terms of
the GNU General Public License <https://www.gnu.org/licenses/gpl.html>.
There is NO WARRANTY, to the extent permitted by law.

Written by Jean-loup Gailly.

[findUncompressedDeflateBlocks] ( 2094 <= 2115 +- 11 <= 2129 ) MB/s
    Block candidates (33):  1641849 14802352 16084952 18358560 19055164 23166984 28247616 32673280 32778929 34162050 37006712 37488520 38504569 43578016 43755016 ...

[findUncompressedDeflateBlocksNestedBranches] ( 1986 <= 2013 +- 13 <= 2031 ) MB/s
    Block candidates (33):  1641857 14802362 16084963 18358568 19055168 23166992 28247625 32673288 32778936 34162056 37006720 37488528 38504576 43578025 43755024 ...

Gzip streams (2):  0 12748064
Deflate blocks (495):  192 205414 411532 617749 824122 1029728 1236300 1442840 1649318 1855554 2061582 2267643 2473676 2679825 2886058 ...

Block size distribution: min: 0 B, avg: 25783.4 B +- 38.8132 B, max: 25888 B
Block Size Distribution (small to large):
     0 b |
         |
         |
         |
         |
207110 b |==================== (494)


[findDeflateBlocksZlib] ( 0.19949 <= 0.19994 +- 0.0003 <= 0.20037 ) MB/s
    Block candidates (20):  192 205414 411532 617749 824122 1028344 1028348 1028349 1029728 1236300 1442840 1572611 1572612 1641846 1641847 ...

[findDeflateBlocksZlibOptimized] ( 0.656 <= 0.665 +- 0.005 <= 0.671 ) MB/s
    Block candidates (11):  192 205414 411532 617749 824122 1029728 1236300 1442840 1649318 1855554 2094939

From 101984512 bits to test, found 10793207 (10.5832 %) candidates and reduced them down further to 494.
Reading dynamic Huffman Code (HC) deflate block failed because the code lengths were invalid:
    Total number of test locations (including those skipped with the jump LUT): 101984512
    Invalid Precode  HC: 10750087 (10.5409 %)
    Invalid Distance HC: 8171 (0.008012 %)
    Invalid Symbol   HC: 76 (7.45211e-05 %)
    Failed checkPrecode calls: 10750087

Filtering cascade:
+-> Total number of test locations: 101984512
    Filtered by deflate header test jump LUT: 91191304 (89.4168 %)
    Remaining locations to test: 10793208
    +-> Failed checkPrecode calls: 10750087 (99.6005 %)
        Remaining locations to test: 43121
        +-> Invalid Distance Huffman Coding: 8171 (18.949 %)
            Remaining locations: 34950
            +-> Failing precode HC usage or literal/distance HC construction: 34456
                Location candidates: 494

Precode CL count:
     4 : 657611
     5 : 658794
     6 : 655427
     7 : 667647
     8 : 656510
     9 : 656660
    10 : 649637
    11 : 705194
    12 : 663377
    13 : 662213
    14 : 659558
    15 : 678194
    16 : 670387
    17 : 681204
    18 : 699319
    19 : 771475

Encountered errors:
     7114739 Constructing a Huffman coding from the given code length sequence failed!
     3643595 The Huffman coding is not optimal!
       28976 Invalid number of literal/length codes!
        5403 Cannot copy last length because this is the first one!
         494 No error.

Block candidates (494):  192 205414 411532 617749 824122 1029728 1236300 1442840 1649318 1855554 2061582 2267643 2473676 2679825 2886058 ...

[findDeflateBlocksPragzip]           ( 4.493 <= 4.5 +- 0.007 <= 4.513 ) MB/s
[findDeflateBlocksPragzipLUT]        ( 50.3 <= 51.5 +- 0.6 <= 52.1 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass] ( 27.02 <= 27.68 +- 0.29 <= 28 ) MB/s

=== Testing with encoder: pigz ===

> pigz --version
pigz 2.6

[findUncompressedDeflateBlocks] ( 2065 <= 2108 +- 20 <= 2129 ) MB/s
    Block candidates (26):  9115355 15453880 17626256 20230960 20959049 32956072 33043921 36063152 36741288 38098897 42646161 44588288 47241200 51683920 53144346 ...

[findUncompressedDeflateBlocksNestedBranches] ( 1981 <= 1999 +- 9 <= 2009 ) MB/s
    Block candidates (26):  9115363 15453888 17626265 20230970 20959057 32956081 33043930 36063160 36741296 38098906 42646169 44588300 47241208 51683930 53144353 ...

Gzip streams (2):  0 12761091
Deflate blocks (1195):  192 102374 205527 308631 411790 515077 618182 721566 797442 797452 797462 797472 900531 1003441 1106502 ...

Block size distribution: min: 0 B, avg: 10679.8 B +- 4498.38 B, max: 12979 B
Block Size Distribution (small to large):
     0 b |===                  (171)
         |
         |
         |
         |==                   (127)
103838 b |==================== (896)


[findDeflateBlocksZlib] ( 0.1938 <= 0.1945 +- 0.0006 <= 0.1954 ) MB/s
    Block candidates (31):  192 102374 205527 234702 234703 234706 234707 308631 411790 515077 618182 721566 797472 900531 1003441 ...

[findDeflateBlocksZlibOptimized] ( 0.644 <= 0.651 +- 0.003 <= 0.656 ) MB/s
    Block candidates (22):  192 102374 205527 308631 411790 515077 618182 721566 797472 900531 1003441 1106502 1209841 1313251 1416637 ...

From 102088728 bits to test, found 10825714 (10.6042 %) candidates and reduced them down further to 1023.
Reading dynamic Huffman Code (HC) deflate block failed because the code lengths were invalid:
    Total number of test locations (including those skipped with the jump LUT): 102088728
    Invalid Precode  HC: 10782516 (10.5619 %)
    Invalid Distance HC: 8095 (0.00792938 %)
    Invalid Symbol   HC: 97 (9.50154e-05 %)
    Failed checkPrecode calls: 10782516

Filtering cascade:
+-> Total number of test locations: 102088728
    Filtered by deflate header test jump LUT: 91263014 (89.3958 %)
    Remaining locations to test: 10825714
    +-> Failed checkPrecode calls: 10782516 (99.601 %)
        Remaining locations to test: 43198
        +-> Invalid Distance Huffman Coding: 8095 (18.7393 %)
            Remaining locations: 35103
            +-> Failing precode HC usage or literal/distance HC construction: 34080
                Location candidates: 1023

Precode CL count:
     4 : 654362
     5 : 659850
     6 : 657378
     7 : 672068
     8 : 656228
     9 : 660807
    10 : 653346
    11 : 707917
    12 : 662677
    13 : 666890
    14 : 660751
    15 : 680790
    16 : 675077
    17 : 683831
    18 : 702215
    19 : 771527

Encountered errors:
     7143923 Constructing a Huffman coding from the given code length sequence failed!
     3646785 The Huffman coding is not optimal!
       28695 Invalid number of literal/length codes!
        5287 Cannot copy last length because this is the first one!
        1023 No error.

Block candidates (1023):  192 102374 205527 308631 411790 515077 618182 721566 797472 900531 1003441 1106502 1209841 1313251 1416637 ...

[findDeflateBlocksPragzip]           ( 4.388 <= 4.412 +- 0.013 <= 4.43 ) MB/s
[findDeflateBlocksPragzipLUT]        ( 41.23 <= 41.69 +- 0.24 <= 42.01 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass] ( 24.5 <= 24.7 +- 0.1 <= 24.83 ) MB/s

=== Testing with encoder: igzip ===

> igzip --version
igzip command line interface 2.30.0

[findUncompressedDeflateBlocks] ( 2073 <= 2104 +- 21 <= 2134 ) MB/s
    Block candidates (25):  9264928 9787856 11847658 25696321 26896376 30346740 32737457 32800465 34399866 36035720 44696189 48931825 49145872 52556250 53613251 ...

[findUncompressedDeflateBlocksNestedBranches] ( 1993 <= 2004 +- 8 <= 2017 ) MB/s
    Block candidates (25):  9264940 9787864 11847666 25696328 26896384 30346744 32737464 32800472 34399872 36035728 44696192 48931835 49145880 52556258 53613256 ...

Gzip streams (2):  0 12669134
Deflate blocks (129):  1136 790905 1580736 2370674 3160686 3950671 4740448 5530378 6321349 7112718 7903168 8692985 9482887 10274151 11065651 ...

Block size distribution: min: 0 B, avg: 98870.4 B +- 77.9492 B, max: 98950 B
Block Size Distribution (small to large):
     0 b |
         |
         |
         |
         |
791606 b |==================== (128)


[findDeflateBlocksZlib] ( 0.1914 <= 0.1949 +- 0.0017 <= 0.1967 ) MB/s
    Block candidates (8):  1136 790905 1139766 1173134 1580736 1702286 1702289 1702290

[findDeflateBlocksZlibOptimized] ( 0.626 <= 0.654 +- 0.011 <= 0.662 ) MB/s
    Block candidates (3):  1136 790905 1580736

From 101353072 bits to test, found 10586575 (10.4452 %) candidates and reduced them down further to 128.
Reading dynamic Huffman Code (HC) deflate block failed because the code lengths were invalid:
    Total number of test locations (including those skipped with the jump LUT): 101353072
    Invalid Precode  HC: 10545136 (10.4044 %)
    Invalid Distance HC: 7810 (0.00770574 %)
    Invalid Symbol   HC: 67 (6.61055e-05 %)
    Failed checkPrecode calls: 10545136

Filtering cascade:
+-> Total number of test locations: 101353072
    Filtered by deflate header test jump LUT: 90766496 (89.5548 %)
    Remaining locations to test: 10586576
    +-> Failed checkPrecode calls: 10545136 (99.6086 %)
        Remaining locations to test: 41440
        +-> Invalid Distance Huffman Coding: 7810 (18.8465 %)
            Remaining locations: 33630
            +-> Failing precode HC usage or literal/distance HC construction: 33502
                Location candidates: 128

Precode CL count:
     4 : 642651
     5 : 642716
     6 : 643006
     7 : 649977
     8 : 642713
     9 : 643132
    10 : 643990
    11 : 667510
    12 : 648788
    13 : 648831
    14 : 648332
    15 : 653029
    16 : 658401
    17 : 661059
    18 : 685709
    19 : 806731

Encountered errors:
     6971630 Constructing a Huffman coding from the given code length sequence failed!
     3581383 The Huffman coding is not optimal!
       28226 Invalid number of literal/length codes!
        5208 Cannot copy last length because this is the first one!
         128 No error.

Block candidates (128):  1136 790905 1580736 2370674 3160686 3950671 4740448 5530378 6321349 7112718 7903168 8692985 9482887 10274151 11065651 ...

[findDeflateBlocksPragzip]           ( 4.33 <= 4.5 +- 0.09 <= 4.61 ) MB/s
[findDeflateBlocksPragzipLUT]        ( 59.2 <= 61.1 +- 0.8 <= 62.2 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass] ( 29.2 <= 30 +- 0.4 <= 30.4 ) MB/s

=== Testing with encoder: bgzip ===

> bgzip --version
bgzip (htslib) 1.13+ds
Copyright (C) 2021 Genome Research Ltd.

[findUncompressedDeflateBlocks] ( 2041 <= 2078 +- 27 <= 2120 ) MB/s
    Block candidates (32):  2959288 3061001 3075096 6140777 11709712 12621274 12934273 13868528 22428648 27137769 30108368 30328864 34426556 35504328 43584170 ...

[findUncompressedDeflateBlocksNestedBranches] ( 1923 <= 1964 +- 25 <= 1997 ) MB/s
    Block candidates (32):  2959296 3061008 3075104 6140784 11709720 12621280 12934282 13868536 22428661 27137776 30108376 30328875 34426562 35504336 43584176 ...

Got 6 B of FEXTRA field!
Gzip streams (260):  0 49330 98651 147979 197311 246645 295981 345312 394654 443992 493330 542663 591980 641320 690645 ...
Deflate blocks (259):  144 394784 789352 1183976 1578632 1973304 2367992 2762640 3157376 3552080 3946784 4341448 4735984 5130704 5525304 ...

Block size distribution: min: 0 B, avg: 49140.7 B +- 3056.03 B, max: 49347 B
Block Size Distribution (small to large):
     0 b |                     (1)
         |
         |
         |
         |
394776 b |==================== (257)


[findDeflateBlocksZlib] ( 0.1927 <= 0.1938 +- 0.0007 <= 0.1948 ) MB/s
    Block candidates (6):  144 394784 789352 1183976 1578632 1973304

From 101426648 bits to test, found 10685794 (10.5355 %) candidates and reduced them down further to 0.
Reading dynamic Huffman Code (HC) deflate block failed because the code lengths were invalid:
    Total number of test locations (including those skipped with the jump LUT): 101426648
    Invalid Precode  HC: 10643875 (10.4942 %)
    Invalid Distance HC: 7994 (0.00788156 %)
    Invalid Symbol   HC: 86 (8.47903e-05 %)
    Failed checkPrecode calls: 10643875

Filtering cascade:
+-> Total number of test locations: 101426648
    Filtered by deflate header test jump LUT: 90740854 (89.4645 %)
    Remaining locations to test: 10685794
    +-> Failed checkPrecode calls: 10643875 (99.6077 %)
        Remaining locations to test: 41919
        +-> Invalid Distance Huffman Coding: 7994 (19.0701 %)
            Remaining locations: 33925
            +-> Failing precode HC usage or literal/distance HC construction: 33925
                Location candidates: 0

Precode CL count:
     4 : 649877
     5 : 652413
     6 : 649876
     7 : 656779
     8 : 649155
     9 : 653706
    10 : 649399
    11 : 685360
    12 : 654690
    13 : 655229
    14 : 653321
    15 : 671557
    16 : 664263
    17 : 670322
    18 : 689720
    19 : 780127

Encountered errors:
     7043469 Constructing a Huffman coding from the given code length sequence failed!
     3608486 The Huffman coding is not optimal!
       28615 Invalid number of literal/length codes!
        5224 Cannot copy last length because this is the first one!

Block candidates (0):

[findDeflateBlocksPragzip]           ( 4.552 <= 4.583 +- 0.02 <= 4.608 ) MB/s
[findDeflateBlocksPragzipLUT]        ( 65.6 <= 66.6 +- 0.5 <= 67.3 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass] ( 31.05 <= 31.34 +- 0.15 <= 31.49 ) MB/s
[findBgzStreams] ( 12000 <= 32000 +- 7000 <= 36000 ) MB/s
    Block candidates (259):  144 394784 789352 1183976 1578632 1973304 2367992 2762640 3157376 3552080 3946784 4341448 4735984 5130704 5525304 ...

Found 259 gzip stream candidates!
 0 49330 98651 147979 197311 246645 295981 345312 394654 443992 493330 542663 591980 641320 690645 ...


=== Testing with encoder: Python3 gzip ===

> python3 --version
Python 3.10.4

[findUncompressedDeflateBlocks] ( 2068 <= 2092 +- 15 <= 2107 ) MB/s
    Block candidates (27):  194240 942224 2600937 4563939 7237456 9078080 14882897 16489920 23810345 29007536 30927412 33606996 47122649 49244473 49721738 ...

[findUncompressedDeflateBlocksNestedBranches] ( 1931 <= 1987 +- 24 <= 2011 ) MB/s
    Block candidates (27):  194248 942233 2600944 4563944 7237464 9078091 14882904 16489928 23810352 29007546 30927416 33607004 47122656 49244480 49721745 ...

Got 6 B of FEXTRA field!
Gzip streams (2):  0 12759547
Deflate blocks (989):  192 102672 205833 308639 411748 515132 618285 721612 824892 928415 1031456 1134888 1238197 1341253 1444122 ...

Block size distribution: min: 0 B, avg: 12903 B +- 27.2736 B, max: 12999 B
Block Size Distribution (small to large):
     0 b |
         |
         |
         |
         |
103999 b |==================== (988)


[findDeflateBlocksZlib] ( 0.1914 <= 0.1941 +- 0.001 <= 0.1949 ) MB/s
    Block candidates (29):  192 102672 194239 194240 194241 194242 194245 205833 308639 411748 515132 618285 721612 824892 928415 ...

[findDeflateBlocksZlibOptimized] ( 0.6473 <= 0.6506 +- 0.002 <= 0.6526 ) MB/s
    Block candidates (20):  192 102672 205833 308639 411748 515132 618285 721612 824892 928415 1031456 1134888 1238197 1341253 1444122 ...

From 102076376 bits to test, found 10826047 (10.6058 %) candidates and reduced them down further to 988.
Reading dynamic Huffman Code (HC) deflate block failed because the code lengths were invalid:
    Total number of test locations (including those skipped with the jump LUT): 102076376
    Invalid Precode  HC: 10782741 (10.5634 %)
    Invalid Distance HC: 8056 (0.00789213 %)
    Invalid Symbol   HC: 92 (9.01286e-05 %)
    Failed checkPrecode calls: 10782741

Filtering cascade:
+-> Total number of test locations: 102076376
    Filtered by deflate header test jump LUT: 91250328 (89.3942 %)
    Remaining locations to test: 10826048
    +-> Failed checkPrecode calls: 10782741 (99.6 %)
        Remaining locations to test: 43307
        +-> Invalid Distance Huffman Coding: 8056 (18.6021 %)
            Remaining locations: 35251
            +-> Failing precode HC usage or literal/distance HC construction: 34263
                Location candidates: 988

Precode CL count:
     4 : 651276
     5 : 660951
     6 : 658117
     7 : 672607
     8 : 657614
     9 : 659431
    10 : 652931
    11 : 708769
    12 : 661425
    13 : 667119
    14 : 661341
    15 : 682006
    16 : 676820
    17 : 682065
    18 : 702720
    19 : 770855

Encountered errors:
     7144247 Constructing a Huffman coding from the given code length sequence failed!
     3646642 The Huffman coding is not optimal!
       28850 Invalid number of literal/length codes!
        5320 Cannot copy last length because this is the first one!
         988 No error.

Block candidates (988):  192 102672 205833 308639 411748 515132 618285 721612 824892 928415 1031456 1134888 1238197 1341253 1444122 ...

[findDeflateBlocksPragzip]           ( 4.385 <= 4.415 +- 0.014 <= 4.429 ) MB/s
[findDeflateBlocksPragzipLUT]        ( 42.02 <= 42.33 +- 0.2 <= 42.64 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass] ( 24.54 <= 24.66 +- 0.1 <= 24.85 ) MB/s

=== Testing with encoder: Python3 pgzip ===

> python3 -m pip show pgzip
Name: pgzip
Version: 0.3.1
Summary: A multi-threading implementation of Python gzip module
Home-page: https://github.com/pgzip/pgzip
Author: pgzip team
Author-email: pgzip@thegoldfish.org
License: MIT
Location: /home/hypatia/.local/lib/python3.10/site-packages
Requires:
Required-by:

[findUncompressedDeflateBlocks] ( 2073 <= 2089 +- 11 <= 2104 ) MB/s
    Block candidates (23):  2311842 13547514 17302848 21375988 29568384 38396442 40876281 45811441 46230384 50735041 50861242 51689824 52966512 56154593 62322161 ...

[findUncompressedDeflateBlocksNestedBranches] ( 1970 <= 1994 +- 12 <= 2014 ) MB/s
    Block candidates (23):  2311849 13547520 17302857 21375992 29568395 38396449 40876289 45811448 46230393 50735049 50861248 51689833 52966520 56154600 62322168 ...

Got 8 B of FEXTRA field!
Gzip streams (2):  0 12747800
Deflate blocks (495):  272 205800 411533 617885 824269 1030628 1237131 1442923 1649106 1855109 2061199 2267938 2473926 2680186 2886437 ...

Block size distribution: min: 0 B, avg: 25782.9 B +- 37.5362 B, max: 25890 B
Block Size Distribution (small to large):
     0 b |
         |
         |
         |
         |
207124 b |==================== (494)


[findDeflateBlocksZlib] ( 0.193 <= 0.1942 +- 0.0006 <= 0.1949 ) MB/s
    Block candidates (12):  272 205800 411533 617885 824269 1030628 1164656 1237131 1442923 1649106 1771228 1855109

[findDeflateBlocksZlibOptimized] ( 0.646 <= 0.652 +- 0.003 <= 0.656 ) MB/s
    Block candidates (10):  272 205800 411533 617885 824269 1030628 1237131 1442923 1649106 1855109

From 101982400 bits to test, found 10792864 (10.5831 %) candidates and reduced them down further to 494.
Reading dynamic Huffman Code (HC) deflate block failed because the code lengths were invalid:
    Total number of test locations (including those skipped with the jump LUT): 101982400
    Invalid Precode  HC: 10750230 (10.5413 %)
    Invalid Distance HC: 8081 (0.00792392 %)
    Invalid Symbol   HC: 74 (7.25615e-05 %)
    Failed checkPrecode calls: 10750230

Filtering cascade:
+-> Total number of test locations: 101982400
    Filtered by deflate header test jump LUT: 91189535 (89.4169 %)
    Remaining locations to test: 10792865
    +-> Failed checkPrecode calls: 10750230 (99.605 %)
        Remaining locations to test: 42635
        +-> Invalid Distance Huffman Coding: 8081 (18.9539 %)
            Remaining locations: 34554
            +-> Failing precode HC usage or literal/distance HC construction: 34060
                Location candidates: 494

Precode CL count:
     4 : 656509
     5 : 660442
     6 : 654292
     7 : 669369
     8 : 656014
     9 : 657507
    10 : 647619
    11 : 707401
    12 : 661996
    13 : 660533
    14 : 659182
    15 : 677181
    16 : 671807
    17 : 679678
    18 : 699874
    19 : 773460

Encountered errors:
     7114228 Constructing a Huffman coding from the given code length sequence failed!
     3644157 The Huffman coding is not optimal!
       28689 Invalid number of literal/length codes!
        5296 Cannot copy last length because this is the first one!
         494 No error.

Block candidates (494):  272 205800 411533 617885 824269 1030628 1237131 1442923 1649106 1855109 2061199 2267938 2473926 2680186 2886437 ...

[findDeflateBlocksPragzip]           ( 4.475 <= 4.498 +- 0.012 <= 4.515 ) MB/s
[findDeflateBlocksPragzipLUT]        ( 51.47 <= 51.79 +- 0.22 <= 52.08 ) MB/s
[findDeflateBlocksPragzipLUTTwoPass] ( 27.51 <= 27.72 +- 0.1 <= 27.85 ) MB/s
*/
