#include "GzipReader.hpp"

#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>

#include <crc32.hpp>
#include <DecodedDataView.hpp>
#include <FileUtils.hpp>
#include <filereader/FileReader.hpp>
#include <filereader/Standard.hpp>
#include <pragzip.hpp>

#ifdef WITH_PYTHON_SUPPORT
    #include <filereader/Python.hpp>
#endif


namespace pragzip
{
size_t
GzipReader::read( const int     outputFileDescriptor,
                  char* const   outputBuffer,
                  const size_t  nBytesToRead,
                  StoppingPoint stoppingPoint )
{
    const auto writeFunctor =
        [nBytesDecoded = uint64_t( 0 ), outputFileDescriptor, outputBuffer]
        ( const void* const buffer,
          uint64_t    const size ) mutable
        {
            auto* const currentBufferPosition = outputBuffer == nullptr ? nullptr : outputBuffer + nBytesDecoded;
            /**
             * @note We cannot splice easily here because we don't use std::shared_ptr for the data and therefore
             *       cannot easily extend the lifetime of the spliced data as necessary. It also isn't as
             *       important as for the multi-threaded version because decoding is the bottlneck for the
             *       sequential version.
             */
            ::writeAll( outputFileDescriptor, currentBufferPosition, buffer, size );
            nBytesDecoded += size;
        };

    return read( writeFunctor, nBytesToRead, stoppingPoint );
}

size_t
GzipReader::read( const WriteFunctor& writeFunctor,
                  const size_t        nBytesToRead,
                  const StoppingPoint stoppingPoint )
{
    size_t nBytesDecoded = 0;

    /* This loop is basically a state machine over m_currentPoint and will process different things
     * depending on m_currentPoint and after each processing step it needs to recheck for EOF!
     * First read metadata so that even with nBytesToRead == 0, the position can be advanced over those. */
    while ( !m_bitReader.eof() && !eof() ) {
        if ( !m_currentPoint.has_value() || ( *m_currentPoint == StoppingPoint::END_OF_BLOCK_HEADER ) ) {
            const auto nBytesDecodedInStep = readBlock( writeFunctor, nBytesToRead - nBytesDecoded );

            nBytesDecoded += nBytesDecodedInStep;
            m_streamBytesCount += nBytesDecodedInStep;

            /* After this call to readBlock, m_currentPoint is either unchanged END_OF_BLOCK_HEADER,
             * std::nullopt (block not fully read) or END_OF_BLOCK. In the last case, we should try to read
             * possible gzip footers and headers even if we already have the requested amount of bytes. */

            if ( !m_currentPoint.has_value() || ( *m_currentPoint == StoppingPoint::END_OF_BLOCK_HEADER ) ) {
                if ( nBytesDecoded >= nBytesToRead ) {
                    break;
                }

                if ( nBytesDecodedInStep == 0 ) {
                    /* We did not advance after the readBlock call and did not even read any amount of bytes.
                     * Something went wrong with flushing. Break to avoid infinite loop. */
                    break;
                }
            }
        } else {
            /* This else branch only handles headers and footers and will always advance
             * the current point while not actually decoding any bytes. */
            switch ( *m_currentPoint )
            {
            case StoppingPoint::NONE:
            case StoppingPoint::END_OF_STREAM:
                readGzipHeader();
                break;

            case StoppingPoint::END_OF_STREAM_HEADER:
            case StoppingPoint::END_OF_BLOCK:
                if ( m_currentDeflateBlock.has_value() && m_currentDeflateBlock->eos() ) {
                    readGzipFooter();
                } else {
                    readBlockHeader();
                }
                break;

            case StoppingPoint::END_OF_BLOCK_HEADER:
                assert( false && "Should have been handled before the switch!" );
                break;

            case StoppingPoint::ALL:
                assert( false && "Should only be specified by the user not appear internally!" );
                break;
            }
        }

    #ifdef WITH_PYTHON_SUPPORT
        checkPythonSignalHandlers();
    #endif

        if ( m_currentPoint.has_value() && testFlags( *m_currentPoint, stoppingPoint ) ) {
            break;
        }
    }

    m_currentPosition += nBytesDecoded;
    return nBytesDecoded;
}

void
GzipReader::readBlockHeader()
{
    if ( !m_currentDeflateBlock.has_value() ) {
        throw std::logic_error( "Call readGzipHeader before calling readBlockHeader!" );
    }
    const auto error = m_currentDeflateBlock->readHeader( m_bitReader );
    if ( error != pragzip::Error::NONE ) {
        std::stringstream message;
        message << "Encountered error: " << pragzip::toString( error ) << " while trying to read deflate header!";
        throw std::domain_error( std::move( message ).str() );
    }
    m_currentPoint = StoppingPoint::END_OF_BLOCK_HEADER;
}

void
GzipReader::readGzipHeader()
{
    const auto [header, error] = pragzip::gzip::readHeader( m_bitReader );
    if ( error != pragzip::Error::NONE ) {
        std::stringstream message;
        message << "Encountered error: " << pragzip::toString( error ) << " while trying to read gzip header!";
        throw std::domain_error( std::move( message ).str() );
    }

    m_lastGzipHeader = std::move( header );
    m_currentDeflateBlock.emplace();
    m_currentDeflateBlock->setInitialWindow();
    m_streamBytesCount = 0;
    m_currentPoint = StoppingPoint::END_OF_STREAM_HEADER;
    m_crc32Calculator.reset();
}


size_t
GzipReader::flushOutputBuffer( const WriteFunctor& writeFunctor,
                               const size_t        maxBytesToFlush )
{
    if ( !m_offsetInLastBuffers.has_value()
         || !m_currentDeflateBlock.has_value()
         || !m_currentDeflateBlock->isValid() ) {
        return 0;
    }

    size_t totalBytesFlushed = 0;
    size_t bufferOffset = 0;
    for ( const auto& buffer : m_lastBlockData.data ) {
        if ( ( *m_offsetInLastBuffers >= bufferOffset ) && ( *m_offsetInLastBuffers < bufferOffset + buffer.size() ) ) {
            const auto offsetInBuffer = *m_offsetInLastBuffers - bufferOffset;
            const auto nBytesToWrite = std::min( buffer.size() - offsetInBuffer, maxBytesToFlush - totalBytesFlushed );

            m_crc32Calculator.update( reinterpret_cast<const char*>( buffer.data() + offsetInBuffer ), nBytesToWrite );

            if ( writeFunctor ) {
                writeFunctor( buffer.data() + offsetInBuffer, nBytesToWrite );
            }

            *m_offsetInLastBuffers += nBytesToWrite;
            totalBytesFlushed += nBytesToWrite;
        }

        bufferOffset += buffer.size();
    }

    /* Reset optional offset if end has been reached. */
    size_t totalBufferSize = 0;
    for ( const auto& buffer : m_lastBlockData.data ) {
        totalBufferSize += buffer.size();
    }
    if ( m_offsetInLastBuffers >= totalBufferSize ) {
        m_offsetInLastBuffers = std::nullopt;
    }

    return totalBytesFlushed;
}


size_t
GzipReader::readBlock( const WriteFunctor& writeFunctor,
                       const size_t        nMaxBytesToDecode )
{
    if ( eof() || ( nMaxBytesToDecode == 0 ) ) {
        return 0;
    }

    /* Try to flush remnants in output buffer from interrupted last call. */
    size_t nBytesDecoded = flushOutputBuffer( writeFunctor, nMaxBytesToDecode );
    if ( !bufferHasBeenFlushed() ) {
        return nBytesDecoded;
    }

    while ( true ) {
        if ( bufferHasBeenFlushed() ) {
            if ( !m_currentDeflateBlock.has_value() || !m_currentDeflateBlock->isValid() ) {
                throw std::logic_error( "Call readGzipHeader and readBlockHeader before calling readBlock!" );
            }

            if ( m_currentDeflateBlock->eob() ) {
                m_currentPoint = StoppingPoint::END_OF_BLOCK;
                return nBytesDecoded;
            }

            /* Decode more data from current block. It can then be accessed via Block::lastBuffers. */
            auto error = Error::NONE;
            std::tie( m_lastBlockData, error ) =
                m_currentDeflateBlock->read( m_bitReader, std::numeric_limits<size_t>::max() );
            if ( error != pragzip::Error::NONE ) {
                std::stringstream message;
                message << "Encountered error: " << pragzip::toString( error ) << " while decompressing deflate block.";
                throw std::domain_error( std::move( message ).str() );
            }

            if ( ( m_lastBlockData.size() == 0 ) && !m_currentDeflateBlock->eob() ) {
                throw std::logic_error( "Could not read anything so it should be the end of the block!" );
            }
            m_offsetInLastBuffers = 0;
        }

        if ( nBytesDecoded >= nMaxBytesToDecode ) {
            break;
        }

        m_currentPoint = {};

        const auto flushedCount = flushOutputBuffer( writeFunctor, nMaxBytesToDecode - nBytesDecoded );

        if ( ( flushedCount == 0 ) && !bufferHasBeenFlushed() ) {
            /* Something went wrong with flushing and this would lead to an infinite loop. */
            break;
        }
        nBytesDecoded += flushedCount;
    }

    return nBytesDecoded;
}


void
GzipReader::readGzipFooter()
{
    const auto footer = pragzip::gzip::readFooter( m_bitReader );

    if ( static_cast<uint32_t>( m_streamBytesCount ) != footer.uncompressedSize ) {
        std::stringstream message;
        message << "Mismatching size (" << static_cast<uint32_t>( m_streamBytesCount ) << " <-> footer: "
                << footer.uncompressedSize << ") for gzip stream!";
        throw std::domain_error( std::move( message ).str() );
    }

    if ( !m_currentDeflateBlock.has_value() || !m_currentDeflateBlock->isValid() ) {
        /* A gzip stream should at least contain an end-of-stream block! */
        throw std::logic_error( "Call readGzipHeader and readBlockHeader before readGzipFooter!" );
    }

    m_crc32Calculator.verify( footer.crc32 );

    if ( m_bitReader.eof() ) {
        m_atEndOfFile = true;
    }

    m_currentPoint = StoppingPoint::END_OF_STREAM;
}


[[nodiscard]] std::string
toString( StoppingPoint stoppingPoint )
{
    // *INDENT-OFF*
    switch ( stoppingPoint )
    {
    case StoppingPoint::NONE                 : return "None";
    case StoppingPoint::END_OF_STREAM_HEADER : return "End of Stream Header";
    case StoppingPoint::END_OF_STREAM        : return "End of Stream";
    case StoppingPoint::END_OF_BLOCK_HEADER  : return "End of Block Header";
    case StoppingPoint::END_OF_BLOCK         : return "End of Block";
    case StoppingPoint::ALL                  : return "All";
    };
    return "Unknown";
    // *INDENT-ON*
}
}  // namespace pragzip
