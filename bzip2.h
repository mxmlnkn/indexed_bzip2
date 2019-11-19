/* bzcat.c - decompress stdin to stdout using bunzip2.
 *
 * Copyright 2003, 2007 Rob Landley <rob@landley.net>
 *
 * Based on a close reading (but not the actual code) of the original bzip2
 * decompression code by Julian R Seward (jseward@acm.org), which also
 * acknowledges contributions by Mike Burrows, David Wheeler, Peter Fenwick,
 * Alistair Moffat, Radford Neal, Ian H. Witten, Robert Sedgewick, and
 * Jon L. Bentley.
 *
 * No standard.
 */

#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>


class BitReader
{
public:
    static constexpr size_t IOBUF_SIZE = 4096;
    static constexpr int NO_FILE = -1;

public:
    BitReader( std::string filePath )
    {
        m_file = fopen( filePath.c_str(), "r" );
        initializeFileBuffer( fileno( m_file ) );
    }

    BitReader( int fileDescriptor )
    {
        initializeFileBuffer( fileDescriptor );
    }

    BitReader( const uint8_t* buffer,
               size_t         size )
    {
        initializeFileBuffer( buffer, size );
    }

    ~BitReader()
    {
        if ( m_file != nullptr ) {
            fclose( m_file );
        }
    }

    uint32_t
        read( uint8_t );

private:
    void
    initializeFileBuffer( int fileDescriptor )
    {
        m_fileDescriptor = fileDescriptor;
    }

    void
    initializeFileBuffer( const uint8_t* buffer,
                          size_t         size )
    {
        m_inbuf.resize( size );
        std::memcpy( m_inbuf.data(), buffer, size );
    }

private:
    FILE* m_file = nullptr;
    int m_fileDescriptor = NO_FILE;
    std::vector<uint8_t> m_inbuf;
    uint32_t m_inbufPos = 0;     // stores current position in buffer in bytes

public:
    uint32_t m_inbufBits = 0;    // bitbuffer stores the last read bits from m_inbuf
    uint8_t m_inbufBitCount = 0; // size of bitbuffer in bits
};


inline uint32_t
BitReader::read( const uint8_t bits_wanted )
{
    uint32_t bits = 0;
    assert( bits_wanted <= sizeof( bits ) * 8 );

    // If we need to get more data from the byte buffer, do so.  (Loop getting
    // one byte at a time to enforce endianness and avoid unaligned access.)
    auto bitsNeeded = bits_wanted;
    while ( m_inbufBitCount < bitsNeeded ) {
        // If we need to read more data from file into byte buffer, do so
        if ( m_inbufPos == m_inbuf.size() ) {
            m_inbuf.resize( IOBUF_SIZE );
            const auto nBytesRead = ::read( m_fileDescriptor, m_inbuf.data(), m_inbuf.size() );
            if ( nBytesRead <= 0 ) {
                // this will also happen for invalid file descriptor -1
                throw std::domain_error( "[BitReader] Not enough data to read!" );
            }
            m_inbuf.resize( nBytesRead );
            m_inbufPos = 0;
        }

        // Avoid 32-bit overflow (dump bit buffer to top of output)
        if ( m_inbufBitCount >= 24 ) {
            bits = m_inbufBits & ( ( 1 << m_inbufBitCount ) - 1 );
            bitsNeeded -= m_inbufBitCount;
            bits <<= bitsNeeded;
            m_inbufBitCount = 0;
        }

        // Grab next 8 bits of input from buffer.
        m_inbufBits = ( m_inbufBits << 8 ) | m_inbuf[m_inbufPos++];
        m_inbufBitCount += 8;
    }

    // Calculate result
    m_inbufBitCount -= bitsNeeded;
    bits |= ( m_inbufBits >> m_inbufBitCount ) & ( ( 1 << bitsNeeded ) - 1 );
    assert( bits == ( bits & ( 0xffffffffL >> ( 32 - bits_wanted ) ) ) );
    return bits;
}


class BZ2Reader
{
public:
    /* Constants for huffman coding */
    static constexpr int MAX_GROUPS = 6;
    static constexpr int GROUP_SIZE = 50;       /* 64 would have been more efficient */
    static constexpr int MAX_HUFCODE_BITS = 20; /* Longest huffman code allowed */
    static constexpr int MAX_SYMBOLS = 258;     /* 256 literals + RUNA + RUNB */
    static constexpr int SYMBOL_RUNA = 0;
    static constexpr int SYMBOL_RUNB = 1;

    static constexpr int IOBUF_SIZE = 4096;
    static constexpr int RETVAL_LAST_BLOCK = -100;
    static constexpr int THREADS = 1;
    static constexpr int CRC32_LOOKUP_TABLE_SIZE = 256;
    /* a small lookup table: raw data -> CRC32 value to speed up CRC calculation */
    static const std::array<uint32_t, CRC32_LOOKUP_TABLE_SIZE> CRC32_TABLE;

private:
    // This is what we know about each huffman coding group
    struct GroupData
    {
        std::array<int, MAX_HUFCODE_BITS + 1> limit;
        std::array<int, MAX_HUFCODE_BITS> base;
        std::array<uint16_t, MAX_SYMBOLS> permute;
        uint8_t minLen;
        uint8_t maxLen;
    };

    // Data for burrows wheeler transform
    struct BurrowsWheelerTransformData
    {
        uint32_t origPtr = 0;
        std::array<int, 256> byteCount;
        // State saved when interrupting output
        int writePos = 0;
        int writeRun = 0;
        int writeCount = 0;
        int writeCurrent = 0;
        uint32_t dataCRC = 0; /* CRC of block as calculated by us */
        uint32_t headerCRC = 0; /* what the block data CRC should be */
        std::vector<uint32_t> dbuf;
    };

    // Structure holding all the housekeeping data, including IO buffers and
    // memory that persists between calls to bunzip
    struct bunzip_data
    {
        // Output buffer
        std::array<char, IOBUF_SIZE> outbuf;
        int outbufPos = 0;

        uint32_t totalCRC = 0;

        // First pass decompression data (Huffman and MTF decoding)
        std::array<char, 32768> selectors;                // nSelectors=15 bits
        std::array<GroupData, MAX_GROUPS> groups; // huffman coding tables
        int groupCount = 0;

        // Second pass decompression data (burrows-wheeler transform)
        unsigned int dbufSize = 0;
    };

    struct BlockHeader
    {
        uint64_t magicBytes;
        bool isRandomized;
        /**
         * The Mapping table itself is compressed in two parts:
         * huffman_used_map: each bit indicates whether the corresponding range [0...15], [16...31] is present.
         * huffman_used_bitmaps: 0-16 16-bit bitmaps
         * The Huffman map gives 0, 10, 11, 100, 101, ... (8-bit) symbols
         * Instead of storing 2 * 256 bytes ( 0b : A, 10b : B, .. ) for the table, the first part is left out.
         * And for short maps, only the first n-th are actually stored.
         * The second half is also assumed to be ordered, so that we only need to store which symbols are actually
         * present.
         * This however means that the Huffmann table can't be frequency sorted, therefore this is done in a
         * second step / table, the mtfSymbol (move to front) map.
         * This would need 256 bits to store the table in huffman_used_bitmaps.
         * These bits are split in groups of 16 and the presence of each group is encoded in huffman_used_map
         * to save even more bytes.
         * @verbatim
         *  10001000 00000000     # huffman_used_map (bit map)
         *  ^   ^
         *  |   [64,95]
         *  [0...15]
         *  00000000 00100000     # huffman_used_bitmaps[0]
         *  ^          ^    ^
         *  0          10   15
         *          (newline)
         *  00000100 10001001     # huffman_used_bitmaps[1]
         *  ^    ^   ^   ^  ^
         *  64   69  72  76 95
         *       E   H   L  O
         * @endverbatim
         */
        uint16_t huffman_used_map;
        /**
         * mapping table: if some byte values are never used (encoding things
         * like ascii text), the compression code removes the gaps to have fewer
         * symbols to deal with, and writes a sparse bitfield indicating which
         * values were present.  We make a translation table to convert the symbols
         * back to the corresponding bytes.
         */
        std::array<uint8_t, 256> symbolToByte;
        std::array<uint8_t, 256> mtfSymbol;
        unsigned int symbolCount;
        static_assert( std::numeric_limits<decltype( symbolCount )>::max() >= MAX_SYMBOLS );
        uint16_t huffman_groups; // only actually 15 bit
        /**
         * Every GROUP_SIZE many symbols we switch huffman coding tables.
         * Each group has a selector, which is an index into the huffman coding table arrays.
         *
         * Read in the group selector array, which is stored as MTF encoded
         * bit runs.  (MTF = Move To Front.  Every time a symbol occurs it's moved
         * to the front of the table, so it has a shorter encoding next time.)
         */
        uint16_t selectors_used;
        BurrowsWheelerTransformData bwdata;
    };

public:
    BZ2Reader( const std::string& filePath ) :
        m_bitReader( filePath )
    {
        startBunzip();
    }

    BZ2Reader( const int fileDescriptor ) :
        m_bitReader( fileDescriptor )
    {
        startBunzip();
    }

    BZ2Reader( const char*  bz2Data,
               const size_t size ) :
        m_bitReader( reinterpret_cast<const uint8_t*>( bz2Data ), size )
    {
        startBunzip();
    }

    uint32_t
    streamCRC() const
    {
        return header.bwdata.headerCRC; // only true when called after last block read
    }

    uint32_t
    totalCRC() const
    {
        return bd.totalCRC;
    }
    // Decompress a block of text to intermediate buffer
    int
    readNextBlock();
    int
    decodeBuffer( BurrowsWheelerTransformData* bw,
                  int                          out_fd,
                  char*                        outbuf,
                  int                          len );

    int
    writeData( const int   outputFileDescriptor,
               char* const outputBuffer = nullptr,
               const int   outputBufferSize = 0 )
    {
        const auto rc = decodeBuffer( &header.bwdata, outputFileDescriptor, outputBuffer, outputBufferSize );
        flushOutputBuffer( outputFileDescriptor );
        if ( ( rc == RETVAL_LAST_BLOCK ) && ( header.bwdata.headerCRC == bd.totalCRC ) ) {
            return 0;
        }
        return rc;
    }

private:
    uint32_t
    getBits( uint8_t nBits )
    {
        return m_bitReader.read( nBits );
    }
    void
    flushOutputBuffer( int outputFileDescriptor );
    void
    prepareBurrowsWheeler( BurrowsWheelerTransformData* bw );
    int
    readBlockHeader( BurrowsWheelerTransformData* bw );
    int
    readBlockData( BurrowsWheelerTransformData* bw );
    static std::array<uint32_t, CRC32_LOOKUP_TABLE_SIZE>
    createCRC32LookupTable( bool littleEndian = false );
    void
    startBunzip();

private:
    BitReader m_bitReader;
    BlockHeader header;
    bunzip_data bd;
};


const std::array<uint32_t, BZ2Reader::CRC32_LOOKUP_TABLE_SIZE> BZ2Reader::CRC32_TABLE = createCRC32LookupTable();


/* Read block header at start of a new compressed data block.  Consists of:
 *
 * 48 bits : Block signature, either pi (data block) or e (EOF block).
 * 32 bits : bw->headerCRC
 * 1  bit  : obsolete feature flag.
 * 24 bits : origPtr (Burrows-wheeler unwind index, only 20 bits ever used)
 * 16 bits : Mapping table index.
 *[16 bits]: symToByte[symTotal] (Mapping table.  For each bit set in mapping
 *           table index above, read another 16 bits of mapping table data.
 *           If correspondig bit is unset, all bits in that mapping table
 *           section are 0.)
 *  3 bits : groupCount (how many huffman tables used to encode, anywhere
 *           from 2 to MAX_GROUPS)
 * variable: hufGroup[groupCount] (MTF encoded huffman table data.)
 */
inline int
BZ2Reader::readBlockHeader( BurrowsWheelerTransformData* bw )
{
    header.magicBytes = ( (uint64_t)getBits( 24 ) << 24 ) | (uint64_t)getBits( 24 );
    bw->headerCRC = getBits( 32 );
    if ( header.magicBytes == 0x177245385090 /* bcd(sqrt(pi)) */ ) {
        /* EOF block contains CRC for whole stream? */
        return RETVAL_LAST_BLOCK;
    }

    if ( header.magicBytes != 0x314159265359 /* bcd(pi) */ ) {
        std::stringstream msg;
        msg << "[BZip2 block header] invalid compressed magic 0x" << std::hex << header.magicBytes;
        throw std::domain_error( msg.str() );
    }

    header.isRandomized = getBits( 1 );
    if ( header.isRandomized ) {
        throw std::domain_error( "[BZip2 block header] deprecated isRandomized bit is not supported" );
    }

    if ( ( bw->origPtr = getBits( 24 ) ) > bd.dbufSize ) {
        std::stringstream msg;
        msg << "[BZip2 block header] origPtr " << bw->origPtr << " is larger than buffer size: " << bd.dbufSize;
        throw std::logic_error( msg.str() );
    }

    // mapping table: if some byte values are never used (encoding things
    // like ascii text), the compression code removes the gaps to have fewer
    // symbols to deal with, and writes a sparse bitfield indicating which
    // values were present.  We make a translation table to convert the symbols
    // back to the corresponding bytes.
    header.huffman_groups = getBits( 16 );
    header.symbolCount = 0;
    for ( int i = 0; i < 16; i++ ) {
        if ( header.huffman_groups & ( 1 << ( 15 - i ) ) ) {
            const auto bitmap = getBits( 16 );
            for ( int j = 0; j < 16; j++ ) {
                if ( bitmap & ( 1 << ( 15 - j ) ) ) {
                    header.symbolToByte[header.symbolCount++] = ( 16 * i ) + j;
                }
            }
        }
    }

    // How many different huffman coding groups does this block use?
    bd.groupCount = getBits( 3 );
    if ( ( bd.groupCount < 2 ) || ( bd.groupCount > MAX_GROUPS ) ) {
        std::stringstream msg;
        msg << "[BZip2 block header] Invalid Huffman coding group count " << bd.groupCount;
        throw std::logic_error( msg.str() );
    }

    // nSelectors: Every GROUP_SIZE many symbols we switch huffman coding
    // tables.  Each group has a selector, which is an index into the huffman
    // coding table arrays.
    //
    // Read in the group selector array, which is stored as MTF encoded
    // bit runs.  (MTF = Move To Front.  Every time a symbol occurs it's moved
    // to the front of the table, so it has a shorter encoding next time.)
    if ( !( header.selectors_used = getBits( 15 ) ) ) {
        std::stringstream msg;
        msg << "[BZip2 block header] selectors_used " << header.selectors_used << " is invalid";
        throw std::logic_error( msg.str() );
    }
    for ( int i = 0; i < bd.groupCount; i++ ) {
        header.mtfSymbol[i] = i;
    }
    for ( int i = 0; i < header.selectors_used; i++ ) {
        int j = 0;
        for ( ; getBits( 1 ); j++ ) {
            if ( j >= bd.groupCount ) {
                std::stringstream msg;
                msg << "[BZip2 block header] Could not find zero termination after " << bd.groupCount << " bits";
                throw std::domain_error( msg.str() );
            }
        }

        // Decode MTF to get the next selector, and move it to the front.
        const auto uc = header.mtfSymbol[j];
        memmove( header.mtfSymbol.data() + 1, header.mtfSymbol.data(), j );
        header.mtfSymbol[0] = bd.selectors[i] = uc;
    }

    // Read the huffman coding tables for each group, which code for symTotal
    // literal symbols, plus two run symbols (RUNA, RUNB)
    const auto symCount = header.symbolCount + 2;
    auto& hh = header.huffman_groups;
    for ( int j = 0; j < bd.groupCount; j++ ) {
        // Read lengths
        std::array<uint8_t, MAX_SYMBOLS> length;
        hh = getBits( 5 );
        for ( unsigned int i = 0; i < symCount; i++ ) {
            while ( true ) {
                // !hh || hh > MAX_HUFCODE_BITS in one test.
                if ( MAX_HUFCODE_BITS - 1 < (unsigned)hh - 1 ) {
                    std::stringstream msg;
                    msg << "[BZip2 block header]  start_huffman_length " << hh
                    << " is larger than " << MAX_HUFCODE_BITS << " or zero\n";
                    throw std::logic_error( msg.str() );
                }
                // Grab 2 bits instead of 1 (slightly smaller/faster).  Stop if
                // first bit is 0, otherwise second bit says whether to
                // increment or decrement.
                const auto kk = getBits( 2 );
                if ( kk & 2 ) {
                    hh += 1 - ( ( kk & 1 ) << 1 );
                } else {
                    m_bitReader.m_inbufBitCount++;
                    break;
                }
            }
            length[i] = hh;
        }

        /* Calculate permute[], base[], and limit[] tables from length[].
         *
         * permute[] is the lookup table for converting huffman coded symbols
         * into decoded symbols.  It contains symbol values sorted by length.
         *
         * base[] is the amount to subtract from the value of a huffman symbol
         * of a given length when using permute[].
         *
         * limit[] indicates the largest numerical value a symbol with a given
         * number of bits can have.  It lets us know when to stop reading.
         *
         * To use these, keep reading bits until value <= limit[bitcount] or
         * you've read over 20 bits (error).  Then the decoded symbol
         * equals permute[hufcode_value - base[hufcode_bitcount]].
         */
        const auto hufGroup = &bd.groups[j];
        hufGroup->minLen = *std::min_element( length.begin(), length.begin() + symCount );
        hufGroup->maxLen = *std::max_element( length.begin(), length.begin() + symCount );

        // Note that minLen can't be smaller than 1, so we adjust the base
        // and limit array pointers so we're not always wasting the first
        // entry.  We do this again when using them (during symbol decoding).
        const auto base = hufGroup->base.data() - 1;
        const auto limit = hufGroup->limit.data() - 1;

        // zero temp[] and limit[], and calculate permute[]
        int pp = 0;
        std::array<unsigned int, MAX_HUFCODE_BITS + 1> temp;
        for ( int i = hufGroup->minLen; i <= hufGroup->maxLen; i++ ) {
            temp[i] = limit[i] = 0;
            for ( hh = 0; hh < symCount; hh++ ) {
                if ( length[hh] == i ) {
                    hufGroup->permute[pp++] = hh;
                }
            }
        }

        // Count symbols coded for at each bit length
        for ( unsigned int i = 0; i < symCount; i++ ) {
            temp[length[i]]++;
        }

        /* Calculate limit[] (the largest symbol-coding value at each bit
         * length, which is (previous limit<<1)+symbols at this level), and
         * base[] (number of symbols to ignore at each bit length, which is
         * limit minus the cumulative count of symbols coded for already). */
        pp = hh = 0;
        for ( int i = hufGroup->minLen; i < hufGroup->maxLen; i++ ) {
            pp += temp[i];
            limit[i] = pp - 1;
            pp <<= 1;
            base[i + 1] = pp - ( hh += temp[i] );
        }
        limit[hufGroup->maxLen] = pp + temp[hufGroup->maxLen] - 1;
        limit[hufGroup->maxLen + 1] = INT_MAX;
        base[hufGroup->minLen] = 0;
    }

    return 0;
}


/* First pass, read block's symbols into dbuf[dbufCount].
 *
 * This undoes three types of compression: huffman coding, run length encoding,
 * and move to front encoding.  We have to undo all those to know when we've
 * read enough input.
 */
inline int
BZ2Reader::readBlockData( BurrowsWheelerTransformData* bw )
{
    GroupData* hufGroup = nullptr;

    // We've finished reading and digesting the block header.  Now read this
    // block's huffman coded symbols from the file and undo the huffman coding
    // and run length encoding, saving the result into dbuf[dbufCount++] = uc

    // Initialize symbol occurrence counters and symbol mtf table
    bw->byteCount.fill( 0 );
    for ( size_t i = 0; i < header.mtfSymbol.size(); i++ ) {
        header.mtfSymbol[i] = i;
    }

    // Loop through compressed symbols.  This is the first "tight inner loop"
    // that needs to be micro-optimized for speed.  (This one fills out dbuf[]
    // linearly, staying in cache more, so isn't as limited by DRAM access.)
    int* base = nullptr;
    int* limit = nullptr;
    int dbufCount = 0;
    for ( int ii, jj, hh = 0, runPos = 0, symCount = 0, selector = 0; ; ) {
        // Have we reached the end of this huffman group?
        if ( !( symCount-- ) ) {
            // Determine which huffman coding group to use.
            symCount = GROUP_SIZE - 1;
            if ( selector >= header.selectors_used ) {
                std::stringstream msg;
                msg << "[BZip2 block data] selector " << selector << " out of maximum range " << header.selectors_used;
                throw std::domain_error( msg.str() );
            }
            hufGroup = &bd.groups[bd.selectors[selector++]];
            base = hufGroup->base.data() - 1;
            limit = hufGroup->limit.data() - 1;
        }

        // Read next huffman-coded symbol (into jj).
        ii = hufGroup->minLen;
        jj = getBits( ii );
        while ( jj > limit[ii] ) {
            // if (ii > hufGroup->maxLen) return RETVAL_DATA_ERROR;
            ii++;

            // Unroll getBits() to avoid a function call when the data's in
            // the buffer already.
            const auto kk =
                m_bitReader.m_inbufBitCount ? ( m_bitReader.m_inbufBits >> --( m_bitReader.m_inbufBitCount ) ) &
                1 : getBits( 1 );
            jj = ( jj << 1 ) | kk;
        }
        // Huffman decode jj into nextSym (with bounds checking)
        jj -= base[ii];

        if ( ii > hufGroup->maxLen ) {
            std::stringstream msg;
            msg << "[BZip2 block data] " << ii << " bigger than max length " << hufGroup->maxLen;
            throw std::domain_error( msg.str() );
        }

        if ( (unsigned)jj >= MAX_SYMBOLS ) {
            std::stringstream msg;
            msg << "[BZip2 block data] " << jj << " larger than max symbols " << MAX_SYMBOLS;
            throw std::domain_error( msg.str() );
        }

        const auto nextSym = hufGroup->permute[jj];

        // If this is a repeated run, loop collecting data
        if ( (unsigned)nextSym <= SYMBOL_RUNB ) {
            // If this is the start of a new run, zero out counter
            if ( !runPos ) {
                runPos = 1;
                hh = 0;
            }

            /* Neat trick that saves 1 symbol: instead of or-ing 0 or 1 at
             * each bit position, add 1 or 2 instead. For example,
             * 1011 is 1<<0 + 1<<1 + 2<<2. 1010 is 2<<0 + 2<<1 + 1<<2.
             * You can make any bit pattern that way using 1 less symbol than
             * the basic or 0/1 method (except all bits 0, which would use no
             * symbols, but a run of length 0 doesn't mean anything in this
             * context). Thus space is saved. */
            hh += ( runPos << nextSym ); // +runPos if RUNA; +2*runPos if RUNB
            runPos <<= 1;
            continue;
        }

        /* When we hit the first non-run symbol after a run, we now know
         * how many times to repeat the last literal, so append that many
         * copies to our buffer of decoded symbols (dbuf) now. (The last
         * literal used is the one at the head of the mtfSymbol array.) */
        if ( runPos ) {
            runPos = 0;
            if ( dbufCount + hh > (int)bd.dbufSize ) {
                std::stringstream msg;
                msg << "[BZip2 block data] dbufCount " << dbufCount << " > " << bd.dbufSize << " dbufSize";
                throw std::domain_error( msg.str() );
            }

            const auto uc = header.symbolToByte[header.mtfSymbol[0]];
            bw->byteCount[uc] += hh;
            while ( hh-- ) {
                bw->dbuf[dbufCount++] = uc;
            }
        }

        // Is this the terminating symbol?
        if ( nextSym > header.symbolCount ) {
            break;
        }

        /* At this point, the symbol we just decoded indicates a new literal
         * character. Subtract one to get the position in the MTF array
         * at which this literal is currently to be found. (Note that the
         * result can't be -1 or 0, because 0 and 1 are RUNA and RUNB.
         * Another instance of the first symbol in the mtf array, position 0,
         * would have been handled as part of a run.) */
        if ( dbufCount >= (int)bd.dbufSize ) {
            std::stringstream msg;
            msg << "[BZip2 block data] dbufCount " << dbufCount << " > " << bd.dbufSize << " dbufSize";
            throw std::domain_error( msg.str() );
        }
        ii = nextSym - 1;
        auto uc = header.mtfSymbol[ii];
        // On my laptop, unrolling this memmove() into a loop shaves 3.5% off
        // the total running time.
        while ( ii-- ) {
            header.mtfSymbol[ii + 1] = header.mtfSymbol[ii];
        }
        header.mtfSymbol[0] = uc;
        uc = header.symbolToByte[uc];

        // We have our literal byte.  Save it into dbuf.
        bw->byteCount[uc]++;
        bw->dbuf[dbufCount++] = uc;
    }

    // Now we know what dbufCount is, do a better sanity check on origPtr.
    if ( bw->origPtr >= (unsigned int)( bw->writeCount = dbufCount ) ) {
        std::stringstream msg;
        msg << "[BZip2 block data] origPtr error " << bw->origPtr;
        throw std::domain_error( msg.str() );
    }

    return 0;
}


inline void
BZ2Reader::flushOutputBuffer( int outputFileDescriptor )
{
    if ( bd.outbufPos ) {
        if ( write( outputFileDescriptor, bd.outbuf.data(), bd.outbufPos ) != bd.outbufPos ) {
            throw std::logic_error( "Could not flush complete output buffer" );
        }
        bd.outbufPos = 0;
    }
}


inline void
BZ2Reader::prepareBurrowsWheeler( BurrowsWheelerTransformData* bw )
{
    // Turn byteCount into cumulative occurrence counts of 0 to n-1.
    for ( size_t i = 0, j = 0; i < bw->byteCount.size(); i++ ) {
        const auto kk = j + bw->byteCount[i];
        bw->byteCount[i] = j;
        j = kk;
    }

    // Use occurrence counts to quickly figure out what order dbuf would be in
    // if we sorted it.
    for ( int i = 0; i < bw->writeCount; i++ ) {
        const auto uc = static_cast<uint8_t>( bw->dbuf[i] );
        bw->dbuf[bw->byteCount[uc]] |= ( i << 8 );
        bw->byteCount[uc]++;
    }

    // blockrandomized support would go here.

    // Using i as position, j as previous character, hh as current character,
    // and uc as run count.
    bw->dataCRC = 0xffffffffL;

    /* Decode first byte by hand to initialize "previous" byte. Note that it
     * doesn't get output, and if the first three characters are identical
     * it doesn't qualify as a run (hence uc=255, which will either wrap
     * to 1 or get reset). */
    if ( bw->writeCount ) {
        bw->writePos = bw->dbuf[bw->origPtr];
        bw->writeCurrent = (unsigned char)bw->writePos;
        bw->writePos >>= 8;
        bw->writeRun = -1;
    }
}


inline int
BZ2Reader::readNextBlock()
{
    int rc = readBlockHeader( &header.bwdata );
    if ( !rc ) {
        rc = readBlockData( &header.bwdata );
    }

    // First thing that can be done by a background thread.
    prepareBurrowsWheeler( &header.bwdata );

    return rc;
}


// Undo burrows-wheeler transform on intermediate buffer to produce output.
// If !len, write up to len bytes of data to buf.  Otherwise write to out_fd.
// Returns len ? bytes written : 0.  Notice all errors are negative #'s.
//
// Burrows-wheeler transform is described at:
// http://dogma.net/markn/articles/bwt/bwt.htm
// http://marknelson.us/1996/09/01/bwt/
inline int
BZ2Reader::decodeBuffer( BurrowsWheelerTransformData* bw,
                         int                          out_fd,
                         char*                        outbuf,
                         int                          len )
{
    int count, pos, current, run, copies, outbyte, previous, gotcount = 0;

    for ( ; ; ) {
        // If last read was short due to end of file, return last block now
        if ( bw->writeCount < 0 ) {
            return bw->writeCount;
        }

        // If we need to refill dbuf, do it.
        if ( !bw->writeCount ) {
            int i = readNextBlock();
            if ( i ) {
                if ( i == RETVAL_LAST_BLOCK ) {
                    bw->writeCount = i;
                    return gotcount;
                } else {
                    return i;
                }
            }
        }

        // loop generating output
        count = bw->writeCount;
        pos = bw->writePos;
        current = bw->writeCurrent;
        run = bw->writeRun;
        while ( count ) {
            // If somebody (like tar) wants a certain number of bytes of
            // data from memory instead of written to a file, humor them.
            if ( len && ( bd.outbufPos >= len ) ) {
                goto dataus_interruptus;
            }
            count--;

            // Follow sequence vector to undo Burrows-Wheeler transform.
            previous = current;
            pos = bw->dbuf[pos];
            current = pos & 0xff;
            pos >>= 8;

            // Whenever we see 3 consecutive copies of the same byte,
            // the 4th is a repeat count
            if ( run++ == 3 ) {
                copies = current;
                outbyte = previous;
                current = -1;
            } else {
                copies = 1;
                outbyte = current;
            }

            // Output bytes to buffer, flushing to file if necessary
            while ( copies-- ) {
                if ( bd.outbufPos == IOBUF_SIZE ) {
                    flushOutputBuffer( out_fd );
                }
                bd.outbuf[bd.outbufPos++] = outbyte;
                bw->dataCRC = ( bw->dataCRC << 8 )
                              ^ CRC32_TABLE[( bw->dataCRC >> 24 ) ^ outbyte];
            }
            if ( current != previous ) {
                run = 0;
            }
        }

        // decompression of this block completed successfully
        bw->dataCRC = ~( bw->dataCRC );
        bd.totalCRC = ( ( bd.totalCRC << 1 ) | ( bd.totalCRC >> 31 ) ) ^ bw->dataCRC;

        // if this block had a crc error, force file level crc error.
        if ( bw->dataCRC != bw->headerCRC ) {
            bd.totalCRC = bw->headerCRC + 1;

            return RETVAL_LAST_BLOCK;
        }
dataus_interruptus:
        bw->writeCount = count;
        if ( len ) {
            gotcount += bd.outbufPos;
            memcpy( outbuf, bd.outbuf.data(), len );

            // If we got enough data, checkpoint loop state and return
            if ( ( len -= bd.outbufPos ) < 1 ) {
                bd.outbufPos -= len;
                if ( bd.outbufPos ) {
                    memmove( bd.outbuf.data(), bd.outbuf.data() + len, bd.outbufPos );
                }
                bw->writePos = pos;
                bw->writeCurrent = current;
                bw->writeRun = run;

                return gotcount;
            }
        }
    }
}


inline void
BZ2Reader::startBunzip()
{
    // Ensure that file starts with "BZh".
    for ( auto i = 0; i < 3; i++ ) {
        if ( getBits( 8 ) != (uint32_t)"BZh"[i] ) {
            throw std::domain_error( "Input header is not BZip2 magic 'BZh'" );
        }
    }

    // Next byte ascii '1'-'9', indicates block size in units of 100k of
    // uncompressed data. Allocate intermediate buffer for block.
    const auto i = getBits( 8 );
    if ( ( i < '1' ) || ( i > '9' ) ) {
        std::stringstream msg;
        msg << "[Open BZip2] Blocksize must be one of '0' (" << std::hex << (int)'0' << ") ... '9' (" << (int)'9'
        << ") but is " << i << " (" << (int)i << ")" << std::dec;
        throw std::domain_error( msg.str() );
    }
    bd.dbufSize = 100000 * ( i - '0' ) * THREADS;
    header.bwdata.dbuf.resize( bd.dbufSize, 0 );
}


inline std::array<uint32_t, BZ2Reader::CRC32_LOOKUP_TABLE_SIZE>
BZ2Reader::createCRC32LookupTable( bool littleEndian )
{
    std::array<uint32_t, CRC32_LOOKUP_TABLE_SIZE> table;
    for ( size_t i = 0; i < table.size(); ++i ) {
        uint32_t c = littleEndian ? i : i << 24;
        for ( int j = 0; j < 8; ++j ) {
            if ( littleEndian ) {
                c = ( c & 1 ) ? ( c >> 1 ) ^ 0xEDB88320 : c >> 1;
            } else {
                c = c & 0x80000000 ? ( c << 1 ) ^ 0x04c11db7 : ( c << 1 );
            }
        }
        table[i] = c;
    }
    return table;
}
