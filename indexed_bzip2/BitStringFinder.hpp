#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <climits>
#include <cstring>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <unistd.h>         // dup, fileno

#include <common.hpp>


/**
 * No matter the input, the data is read from an input buffer.
 * If a file is given, then that input buffer will be refilled when the input buffer empties.
 * It is less a file object and acts more like an iterator.
 * It offers a @ref find method returning the next match or std::numeric_limits<size_t>::max() if the end was reached.
 */
class BitStringFinder
{
public:
    using ShiftedLUTTable = std::vector<std::pair</* shifted value to compare to */ uint64_t, /* mask */ uint64_t> >;

public:
    BitStringFinder( BitStringFinder&& ) = default;

    BitStringFinder( const BitStringFinder& other ) = delete;

    BitStringFinder& operator=( const BitStringFinder& ) = delete;

    BitStringFinder& operator=( BitStringFinder&& ) = delete;

    BitStringFinder( std::string filePath,
                     uint64_t    bitStringToFind,
                     uint8_t     bitStringSize,
                     size_t      fileBufferSizeBytes = 1*1024*1024 ) :
        BitStringFinder( bitStringToFind, bitStringSize, fileBufferSizeBytes )
    {
        m_file = std::fopen( filePath.c_str(), "rb" );
        fseek( m_file, 0, SEEK_SET );
    }

    BitStringFinder( int      fileDescriptor,
                     uint64_t bitStringToFind,
                     uint8_t  bitStringSize,
                     size_t   fileBufferSizeBytes = 1*1024*1024 ) :
        BitStringFinder( bitStringToFind, bitStringSize, fileBufferSizeBytes )
    {
        /** dup is not strong enough to be able to independently seek in the old and the dup'ed fd! */
        m_file = std::fopen( fdFilePath( fileDescriptor ).c_str(), "rb" );
        fseek( m_file, 0, SEEK_SET );
    }

    BitStringFinder( const char* buffer,
                     size_t      size,
                     uint64_t    bitStringToFind,
                     uint8_t     bitStringSize ) :
        BitStringFinder( bitStringToFind, bitStringSize )
    {
        m_buffer.assign( buffer, buffer + size );
    }

    /**
     * @return the next match or std::numeric_limits<size_t>::max() if the end was reached.
     */
    size_t
    find();

private:
    BitStringFinder( uint64_t    bitStringToFind,
                     uint8_t     bitStringSize,
                     size_t      fileBufferSizeBytes = 1*1024*1024 ) :
        m_fileChunksInBytes( std::max( fileBufferSizeBytes,
                                       static_cast<size_t>( ceilDiv( bitStringSize, CHAR_BIT ) ) ) ),
        m_bitStringToFind  ( bitStringToFind & mask( bitStringSize ) ),
        m_bitStringSize    ( bitStringSize ),
        m_movingBitsToKeep ( bitStringSize > 0 ? bitStringSize - 1u : 0u ),
        m_movingBytesToKeep( ceilDiv( m_movingBitsToKeep, CHAR_BIT ) )
    {
        if ( m_movingBytesToKeep >= m_fileChunksInBytes ) {
            std::stringstream msg;
            msg << "The file buffer size of " << m_fileChunksInBytes << "B is too small to look for strings with "
                << m_bitStringSize << " bits!";
            throw std::invalid_argument( msg.str() );
        }
    }

    bool
    eof() const
    {
        if ( m_file != nullptr ) {
            return m_buffer.empty() && std::feof( m_file );
        }
        return m_buffer.empty();
    }

    /**
     * @verbatim
     * 63                48                  32                  16        8         0
     * |                 |                   |                   |         |         |
     * 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 1111 1111 1111
     *                                                                  <------------>
     *                                                                    length = 12
     * @endverbatim
     *
     * @param length the number of lowest bits which should be 1 (rest are 0)
     */
    static constexpr uint64_t
    mask( uint8_t length )
    {
        return ~static_cast<uint64_t>( 0 ) >> ( sizeof( uint64_t ) * CHAR_BIT - length );
    }

    size_t
    refillBuffer();

    static ShiftedLUTTable
    createdShiftedBitStringLUT( uint64_t bitString,
                                uint8_t  bitStringSize,
                                bool     includeLastFullyShifted = false );

    static size_t
    findBitString( const uint8_t* buffer,
                   size_t         bufferSize,
                   uint64_t       bitString,
                   uint8_t        bitStringSize,
                   uint8_t        firstBitsToIgnore = 0 );

    /** dup is not strong enough to be able to independently seek in the old and the dup'ed fd! */
    static std::string
    fdFilePath( int fileDescriptor )
    {
        std::stringstream filename;
        filename << "/proc/self/fd/" << fileDescriptor;
        return filename.str();
    }

private:
    std::FILE* m_file = nullptr;
    /** This is not the current size of @ref m_buffer but the number of bytes to read from @ref m_file if it is empty */
    const size_t m_fileChunksInBytes;
    std::vector<char> m_buffer;
    /**
     * In some way this is the buffer for the input buffer.
     * It is a moving window of m_bitStringSize bits which can be directly compared to m_bitString
     * This moving window also ensure that bit strings at file chunk boundaries are recognized correctly!
     */
    uint64_t m_movingWindow = 0;

    /**
     * How many bits from m_buffer bits are already read. The first bit string comparison will be done
     * after m_nTotalBytesRead * CHAR_BIT + m_bufferBitsRead >= m_bitStringSize
     */
    size_t m_bufferBitsRead = 0;
    /**
     * This value is incremented whenever the buffer is refilled. It basically acts like an overflow counter
     * for @ref m_bufferBitsRead and is required to return the absolute bit pos.
     */
    size_t m_nTotalBytesRead = 0;

    const uint64_t m_bitStringToFind;
    const uint8_t m_bitStringSize;

    /**
     * If the bit string is only one bit long, then we don't need to keep bits from the current buffer.
     * However, for a 2 bit string, one bit might be at the end of the current and the other at the beginning
     * of the next chunk, so we need to keep the last byte of that buffer but then mark the first 7 bits read
     * to avoid returning duplicate offsets!
     * For 8 bits, at worst 7 bits are in the current buffer and 1 in the next, so we need to keep 1 byte
     * and mark 1 bit read in the new buffer.
     * For 9 bits, keep 8 bits (1B) and mark 0 bits read.
     * This gives the formula for the bytes to keep as: ceil( ( bitStringSize - 1 ) / 8 ).
     */
    const uint8_t m_movingBitsToKeep;
    const uint8_t m_movingBytesToKeep;
};


inline BitStringFinder::ShiftedLUTTable
BitStringFinder::createdShiftedBitStringLUT( uint64_t bitString,
                                             uint8_t  bitStringSize,
                                             bool     includeLastFullyShifted )
{
    const auto nWildcardBits = sizeof( uint64_t ) * CHAR_BIT - bitStringSize;
    ShiftedLUTTable shiftedBitStrings( nWildcardBits + includeLastFullyShifted );

    uint64_t shiftedBitString = bitString;
    uint64_t shiftedBitMask = std::numeric_limits<uint64_t>::max() >> nWildcardBits;
    for ( size_t i = 0; i < shiftedBitStrings.size(); ++i ) {
        shiftedBitStrings[shiftedBitStrings.size() - 1 - i] = std::make_pair( shiftedBitString, shiftedBitMask );
        shiftedBitString <<= 1;
        shiftedBitMask   <<= 1;
        assert( ( shiftedBitString & shiftedBitMask ) == shiftedBitString );
    }

    return shiftedBitStrings;
}


/**
 * @param bitString the lowest bitStringSize bits will be looked for in the buffer
 * @return size_t max if not found else position in buffer
 */
inline size_t
BitStringFinder::findBitString( const uint8_t* buffer,
                                size_t         bufferSize,
                                uint64_t       bitString,
                                uint8_t        bitStringSize,
                                uint8_t        firstBitsToIgnore )
{
    /* Simply load bytewise even if we could load more (uneven) bits by rounding down.
     * This makes this implementation much less performant in comparison to the "% 8 = 0" version! */
    const auto nBytesToLoadPerIteration = ( sizeof( uint64_t ) * CHAR_BIT - bitStringSize ) / CHAR_BIT;
    if ( nBytesToLoadPerIteration <= 0 ) {
        throw std::invalid_argument( "Bit string size must be smaller than or equal to 56 bit in order to "
                                     "load bytewise!" );
    }

    /* Initialize buffer window. Note that we can't simply read an uint64_t because of the bit and byte order */
    if ( bufferSize * CHAR_BIT < bitStringSize ) {
        return std::numeric_limits<size_t>::max();
    }
    size_t i = 0;
    uint64_t window = 0;
    for ( ; i < std::min( sizeof( window ), bufferSize ); ++i ) {
        window = ( window << CHAR_BIT ) | buffer[i];
    }

    /* The extra checks on firstBitsToIgnore and foundBitOffset < bufferSize *8 are only necessary here at
     * the beginning because the uint64_t "buffer" might not be all full and firstBitsToIgnore will also only
     * matter for the first 8 bits. But in the tight loop below, these checks slow down the bit string finder
     * from 2.0s to 2.7s! */
    if ( firstBitsToIgnore >= CHAR_BIT ) {
        throw std::invalid_argument( "Only up to 7 bits should be to be ignored. Else incremenent the input "
                                     "buffer pointer!" );
    }
    {
        /* Use pre-shifted search bit string values and masks to test for the search string in the larger window.
         * Only for this very first check is it possible that we have the pattern fully shifted by
         * nBytesToLoadPerIteration. That's why we need to iterate over a bit shift table which has one more
         * entry than the tight loop below needs to have! In later iterations, i.e., in the tight loop down
         * below, this can't happen because then the pattern woudl have been found in one of the prior iterations. */
        size_t k = 0;
        const auto shiftedBitStrings = createdShiftedBitStringLUT( bitString, bitStringSize, true );
        for ( const auto& [shifted, mask] : shiftedBitStrings ) {
            if ( ( window & mask ) == shifted ) {
                const auto foundBitOffset = i * CHAR_BIT - bitStringSize - ( shiftedBitStrings.size() - 1 - k );
                if ( ( foundBitOffset >= firstBitsToIgnore ) && ( foundBitOffset < bufferSize * CHAR_BIT ) ) {
                    return foundBitOffset - firstBitsToIgnore;
                }
            }
            ++k;
        }
    }

    /* This tight loop is the performance critical part! */
    const auto shiftedBitStrings = createdShiftedBitStringLUT( bitString, bitStringSize, false );
    for ( ; i < bufferSize; ) {
        for ( size_t j = 0; ( j < nBytesToLoadPerIteration ) && ( i < bufferSize ); ++i, ++j ) {
            window = ( window << CHAR_BIT ) | buffer[i];
        }

        /* Use pre-shifted search bit string values and masks to test for the search string in the larger window.
         * Note that the order of the shiftedBitStrings matter because we return the first found match! */
        size_t k = 0;
        for ( const auto& [shifted, mask] : shiftedBitStrings ) {
            if ( ( window & mask ) == shifted ) {
                return i * CHAR_BIT - bitStringSize - ( shiftedBitStrings.size() - 1 - k ) - firstBitsToIgnore;
            }
            ++k;
        }
    }

    return std::numeric_limits<size_t>::max();
}


inline size_t
BitStringFinder::find()
{
    if ( m_bitStringSize == 0 ) {
        return std::numeric_limits<size_t>::max();
    }

    while ( !eof() )
    {
        if ( m_bufferBitsRead >= m_buffer.size() * CHAR_BIT ) {
            const auto nBytesRead = refillBuffer();
            if ( nBytesRead == 0 ) {
                return std::numeric_limits<size_t>::max();
            }
        }

        #define ALGO 1
        #if ALGO == 1

        for ( ; m_bufferBitsRead < m_buffer.size() * CHAR_BIT; ) {
            const auto byteOffset = m_bufferBitsRead / CHAR_BIT;
            const auto firstBitsToIgnore = m_bufferBitsRead % CHAR_BIT;

            const auto relpos = findBitString( reinterpret_cast<const uint8_t*>( m_buffer.data() ) + byteOffset,
                                               m_buffer.size() - byteOffset,
                                               m_bitStringToFind, m_bitStringSize, firstBitsToIgnore );
            if ( relpos == std::numeric_limits<size_t>::max() ) {
                m_bufferBitsRead = m_buffer.size() * CHAR_BIT;
                break;
            }

            m_bufferBitsRead += relpos;

            const auto foundOffset = m_nTotalBytesRead * CHAR_BIT + m_bufferBitsRead;
            m_bufferBitsRead += 1;
            return foundOffset;
        }

        #elif ALGO == 2

        const auto bitMask = mask( m_bitStringSize );

        /* Initialize the moving window with m_bitStringSize-1 bits.
         * Note that one additional bit is loaded before the first comparison.
         * At this point, we know that there are at least m_bitStringSize unread bits in the buffer. */
        if ( m_nTotalBytesRead * CHAR_BIT + m_bufferBitsRead < m_bitStringSize-1u ) {
            const auto nBitsToRead = m_bitStringSize-1 - ( m_nTotalBytesRead * CHAR_BIT + m_bufferBitsRead );
            for ( size_t i = 0; i < nBitsToRead; ++i, ++m_bufferBitsRead ) {
                const auto byte = static_cast<unsigned char>( m_buffer[m_bufferBitsRead / CHAR_BIT] );
                const auto bit = ( byte >> ( 7 - ( m_bufferBitsRead & 7U ) ) ) & 1U;
                m_movingWindow = ( ( m_movingWindow << 1 ) | bit ) & bitMask;
            }
        }

        for ( ; m_bufferBitsRead < m_buffer.size() * CHAR_BIT; ) {
            const auto byte = static_cast<unsigned char>( m_buffer[m_bufferBitsRead / CHAR_BIT] );
            for ( int j = m_bufferBitsRead & 7U; j < CHAR_BIT; ++j, ++m_bufferBitsRead ) {
                const auto bit = ( byte >> ( 7 - j ) ) & 1U;
                m_movingWindow = ( ( m_movingWindow << 1 ) | bit ) & bitMask;
                if ( m_movingWindow == m_bitStringToFind )
                {
                    ++m_bufferBitsRead;
                    return m_nTotalBytesRead * CHAR_BIT + m_bufferBitsRead - m_bitStringSize;
                }
            }
        }

        #endif
    }

    return std::numeric_limits<size_t>::max();
}


inline size_t
BitStringFinder::refillBuffer()
{
    if ( m_file == nullptr ) {
        m_nTotalBytesRead += m_buffer.size();
        m_buffer.clear();
        return std::numeric_limits<size_t>::max();
    }

    /* Read chunk of data from file into buffer. */
    size_t nBytesRead = 0;
    if ( m_buffer.empty() ) {
        assert( m_nTotalBytesRead == 0 );
        assert( m_bufferBitsRead == 0 );

        m_buffer.resize( m_fileChunksInBytes );
        nBytesRead = std::fread( m_buffer.data(), 1, m_buffer.size(), m_file );
        m_buffer.resize( nBytesRead );
    } else {
        m_nTotalBytesRead += m_buffer.size() - m_movingBytesToKeep;
        m_bufferBitsRead = m_movingBytesToKeep * CHAR_BIT - m_movingBitsToKeep;

        /* On subsequent refills, keep the last bits in order to find bit strings on buffer boundaries. */
        std::memmove( m_buffer.data(), m_buffer.data() + m_buffer.size() - m_movingBytesToKeep, m_movingBytesToKeep );

        const auto nBytesToRead = m_buffer.size() - m_movingBytesToKeep;
        nBytesRead = std::fread( m_buffer.data() + m_movingBytesToKeep, /* element size */ 1, nBytesToRead, m_file );
        m_buffer.resize( m_movingBytesToKeep + nBytesRead );
    }

    return nBytesRead;
}
