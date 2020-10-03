#pragma once

#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <unistd.h>         // dup, fileno


template<typename I1,
         typename I2,
         typename Enable = typename std::enable_if<
            std::is_integral<I1>::value &&
            std::is_integral<I2>::value
         >::type>
I1
ceilDiv( I1 dividend,
         I2 divisor )
{
    return ( dividend + divisor - 1 ) / divisor;
}


/**
 * No matter the input, the data is read from an input buffer.
 * If a file is given, then that input buffer will be refilled when the input buffer empties.
 * It is less a file object and acts more like an iterator.
 * It offers a @ref find method returning the next match or std::numeric_limits<size_t>::max() if the end was reached.
 */
class BitStringFinder
{
public:
    BitStringFinder( BitStringFinder&& ) = default;

    BitStringFinder( const BitStringFinder& other ) = delete;

    BitStringFinder& operator=( const BitStringFinder& ) = delete;

    BitStringFinder& operator=( BitStringFinder&& ) = default;

    BitStringFinder( std::string filePath,
                     uint64_t    bitStringToFind,
                     uint8_t     bitStringSize,
                     size_t      fileBufferSizeBytes = 1*1024*1024 ) :
        m_file( std::fopen( filePath.c_str(), "rb" ) ),
        m_fileChunksInBytes( std::max( fileBufferSizeBytes, static_cast<size_t>( ceilDiv( bitStringSize, 8 ) ) ) ),
        m_bitStringToFind( bitStringToFind & mask( bitStringSize ) ),
        m_bitStringSize( bitStringSize )
    {
        fseek( m_file, 0, SEEK_SET );
    }

    BitStringFinder( int fileDescriptor,
                     uint64_t    bitStringToFind,
                     uint8_t     bitStringSize,
                     size_t      fileBufferSizeBytes = 1*1024*1024 ) :
        m_file( fdopen( dup( fileDescriptor ), "rb" ) ),
        m_fileChunksInBytes( std::max( fileBufferSizeBytes, static_cast<size_t>( ceilDiv( bitStringSize, 8 ) ) ) ),
        m_bitStringToFind( bitStringToFind & mask( bitStringSize ) ),
        m_bitStringSize( bitStringSize )
    {
        fseek( m_file, 0, SEEK_SET );
    }

    BitStringFinder( const char* buffer,
                     size_t      size,
                     uint64_t    bitStringToFind,
                     uint8_t     bitStringSize ) :
        m_buffer( buffer, buffer + size ),
        m_bitStringToFind( bitStringToFind & mask( bitStringSize ) ),
        m_bitStringSize( bitStringSize )
    {}

    /**
     * @return the next match or std::numeric_limits<size_t>::max() if the end was reached.
     */
    size_t
    find()
    {
        if ( m_bitStringSize == 0 ) {
            return std::numeric_limits<size_t>::max();
        }

        while ( !eof() )
        {
            if ( m_bufferBitsRead >= m_buffer.size() * CHAR_BIT ) {
                m_nTotalBytesRead += m_buffer.size();
                m_bufferBitsRead = 0;
                if ( m_file == nullptr ) {
                    m_buffer.clear();
                    return std::numeric_limits<size_t>::max();
                }

                /* read chunk of data from file and if file end is reached, break loop */
                m_buffer.resize( m_fileChunksInBytes );
                const auto nBytesRead = std::fread( m_buffer.data(), 1, m_buffer.size(), m_file );
                m_buffer.resize( nBytesRead );
                if ( nBytesRead == 0 ) {
                    return std::numeric_limits<size_t>::max();
                }
            }

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
                    if ( m_movingWindow == m_bitStringToFind ) {
                        ++m_bufferBitsRead;
                        return m_nTotalBytesRead * CHAR_BIT + m_bufferBitsRead - m_bitStringSize;
                    }
                }
            }
        }

        return std::numeric_limits<size_t>::max();
    }

private:
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
    static uint64_t
    mask( uint8_t length )
    {
        return ~static_cast<uint64_t>( 0 ) >> ( sizeof( uint64_t ) * CHAR_BIT - length );
    }

    void
    init()
    {
        if ( m_file != nullptr ) {
            std::fseek( m_file, 0, SEEK_END );
            m_fileSizeBytes = std::ftell( m_file );
            std::fseek( m_file, 0, SEEK_SET ); /* good to have even when not getting the size! */
        }
    }

private:
    std::FILE* m_file = nullptr;
    size_t m_fileSizeBytes = 0;
    /** This is not the current size of @ref m_buffer but the number of bytes to read from @ref m_file if it is empty */
    const size_t m_fileChunksInBytes = 1*1024*1024;
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
};
