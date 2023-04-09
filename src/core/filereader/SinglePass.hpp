#pragma once

#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

#include <common.hpp>
#include <FasterVector.hpp>

#include "FileReader.hpp"


/**
 * This FileReader implementation acts like a buffered file reader with infinite buffer.
 * This file reader will only read sequentially from the given input file reader.
 * It buffers all the sequentially read data and enables seeking inside that buffer.
 * It can therefore be used to make non-seekable file readers seekable.
 *
 * In order to reduce memory consumption it also offers an interface to unload everything
 * before a given offset. Seeking back before that offset will throw an exception!
 */
class SinglePassFileReader :
    public FileReader
{
public:
    static constexpr size_t CHUNK_SIZE = 4_Mi;

public:
    explicit
    SinglePassFileReader( UniqueFileReader fileReader ) :
        m_file( std::move( fileReader ) )
    {}

    ~SinglePassFileReader()
    {
        close();
    }

    [[nodiscard]] UniqueFileReader
    clone() const override
    {
        throw std::invalid_argument( "Cloning file reader not allowed because the internal file position "
                                     "should not be modified by multiple owners!" );
    }

    /* Copying is simply not allowed because that might interfere with the file position state, use SharedFileReader! */

    void
    close() override
    {
        m_file.reset();
    }

    [[nodiscard]] bool
    closed() const override
    {
        return !m_file;
    }

    [[nodiscard]] bool
    eof() const override
    {
        return m_underlyingFileEOF && ( m_currentPosition >= m_numberOfBytesRead );
    }

    [[nodiscard]] bool
    fail() const override
    {
        if ( m_file ) {
            return m_file->fail();
        }
        return false;
    }

    [[nodiscard]] int
    fileno() const override
    {
        if ( m_file ) {
            return m_file->fileno();
        }
        throw std::invalid_argument( "Trying to get fileno of an invalid file!" );
    }

    [[nodiscard]] bool
    seekable() const override
    {
        return true;
    }

    [[nodiscard]] size_t
    read( char*  buffer,
          size_t nMaxBytesToRead ) override
    {
        if ( !m_file ) {
            throw std::invalid_argument( "Invalid or file cannot be seeked!" );
        }

        if ( nMaxBytesToRead == 0 ) {
            return 0;
        }

        bufferUpTo( saturatingAddition( m_currentPosition, nMaxBytesToRead ) );

        /* Find start chunk to start reading from. */
        const auto startChunk = m_currentPosition / CHUNK_SIZE;
        size_t nBytesRead{ 0 };
        for ( size_t i = startChunk; ( i < m_buffer.size() ) && ( nBytesRead < nMaxBytesToRead ); ++i ) {
            const auto chunkOffset = i * CHUNK_SIZE;

            const auto* sourceOffset = m_buffer[i].data();
            auto nAvailableBytes = m_buffer[i].size();

            if ( chunkOffset < m_currentPosition ) {
                if ( m_currentPosition - chunkOffset > nAvailableBytes ) {
                    throw std::logic_error( "Calculation of start chunk seems to be wrong!" );
                }

                const auto nBytesToSkip = m_currentPosition - chunkOffset;
                nAvailableBytes -= nBytesToSkip;
                sourceOffset += nBytesToSkip;
            }

            const auto nBytesToCopy = std::min( nAvailableBytes, nMaxBytesToRead - nBytesRead );
            std::memcpy( buffer + nBytesRead, sourceOffset, nBytesToCopy );
            nBytesRead += nBytesToCopy;
        }

        m_currentPosition += nBytesRead;

        return nBytesRead;
    }

    size_t
    seek( long long int offset,
          int           origin = SEEK_SET ) override
    {
        if ( !m_file ) {
            throw std::invalid_argument( "Invalid or file cannot be seeked!" );
        }

        switch ( origin )
        {
        case SEEK_CUR:
            offset = static_cast<long long int>( tell() ) + offset;
            break;
        case SEEK_SET:
            break;
        case SEEK_END:
            bufferUpTo( std::numeric_limits<size_t>::max() );
            offset = static_cast<long long int>( size() ) + offset;
            break;
        }

        bufferUpTo( offset );
        m_currentPosition = static_cast<size_t>( std::max( 0LL, offset ) );

        return m_currentPosition;
    }

    [[nodiscard]] size_t
    size() const override
    {
        return m_underlyingFileEOF ? m_numberOfBytesRead : 0;
    }

    [[nodiscard]] size_t
    tell() const override
    {
        return m_currentPosition;
    }

    void
    clearerr() override
    {
        if ( m_file ) {
            m_file->clearerr();
        }
    }

private:
    void
    bufferUpTo( const size_t untilOffset )
    {
        while ( m_file && !m_underlyingFileEOF && ( m_numberOfBytesRead < untilOffset ) ) {
            /* If the last chunk is already full, create a new empty one. */
            if ( m_buffer.empty() || ( m_buffer.back().size() >= CHUNK_SIZE ) ) {
                m_buffer.emplace_back();
            }

            /* Fill up the last buffer chunk to CHUNK_SIZE. */
            const auto oldChunkSize = m_buffer.back().size();
            m_buffer.back().resize( CHUNK_SIZE );
            const auto nBytesRead = m_file->read( reinterpret_cast<char*>( m_buffer.back().data() + oldChunkSize ),
                                                  m_buffer.back().size() - oldChunkSize );
            m_buffer.back().resize( oldChunkSize + nBytesRead );

            m_numberOfBytesRead += nBytesRead;
            m_underlyingFileEOF = nBytesRead == 0;
        }
    }

protected:
    UniqueFileReader m_file;
    bool m_underlyingFileEOF{ false };

    size_t m_numberOfBytesRead{ 0 };
    std::deque<FasterVector<std::byte> > m_buffer;

    size_t m_currentPosition{ 0 };
};
