#pragma once

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
#include <deflate.hpp>
#include <FileUtils.hpp>
#include <filereader/FileReader.hpp>
#include <filereader/Standard.hpp>
#include <gzip.hpp>

#ifdef WITH_PYTHON_SUPPORT
    #include <filereader/Python.hpp>
#endif


namespace pragzip
{
enum StoppingPoint : uint32_t
{
    NONE                 = 0U,
    END_OF_STREAM_HEADER = 1U << 0U,
    END_OF_STREAM        = 1U << 1U,  // after gzip footer has been read
    END_OF_BLOCK_HEADER  = 1U << 2U,
    END_OF_BLOCK         = 1U << 3U,
    ALL                  = 0xFFFF'FFFFU,
};


/**
 * A strictly sequential gzip interface that can iterate over multiple gzip streams and of course deflate blocks.
 * It cannot seek back nor is it parallelized but it can be used to implement a parallelization scheme.
 */
class GzipReader :
    public FileReader
{
public:
    using DeflateBlock = typename deflate::Block<>;
    using WriteFunctor = std::function<void ( const void*, uint64_t )>;

public:
    explicit
    GzipReader( UniqueFileReader fileReader ) :
        m_bitReader( std::move( fileReader ) )
    {}

#ifdef WITH_PYTHON_SUPPORT
    explicit
    GzipReader( const std::string& filePath ) :
        m_bitReader( std::make_unique<StandardFileReader>( filePath ) )
    {}

    explicit
    GzipReader( int fileDescriptor ) :
        m_bitReader( std::make_unique<StandardFileReader>( fileDescriptor ) )
    {}

    explicit
    GzipReader( PyObject* pythonObject ) :
        m_bitReader( std::make_unique<PythonFileReader>( pythonObject ) )
    {}
#endif

    /* FileReader finals */

    [[nodiscard]] UniqueFileReader
    clone() const final
    {
        throw std::logic_error( "Not implemented!" );
    }

    [[nodiscard]] int
    fileno() const final
    {
        return m_bitReader.fileno();
    }

    [[nodiscard]] bool
    seekable() const final
    {
        return m_bitReader.seekable();
    }

    void
    close() final
    {
        m_bitReader.close();
    }

    [[nodiscard]] bool
    closed() const final
    {
        return m_bitReader.closed();
    }

    [[nodiscard]] bool
    eof() const final
    {
        return m_atEndOfFile;
    }

    [[nodiscard]] bool
    fail() const final
    {
        throw std::logic_error( "Not implemented!" );
    }

    [[nodiscard]] size_t
    tell() const final
    {
        if ( m_atEndOfFile ) {
            return size();
        }
        return m_currentPosition;
    }

    [[nodiscard]] size_t
    size() const final
    {
        if ( m_atEndOfFile ) {
            return m_currentPosition;
        }

        throw std::invalid_argument( "Can't get stream size when not finished reading at least once!" );
    }

    size_t
    seek( long long int /* offset */,
          int           /* origin */ = SEEK_SET ) final
    {
        throw std::logic_error( "Not implemented (yet)!" );
    }

    void
    clearerr() final
    {
        m_bitReader.clearerr();
        m_atEndOfFile = false;
        throw std::invalid_argument( "Not fully tested!" );
    }

    [[nodiscard]] size_t
    read( char*  outputBuffer,
          size_t nBytesToRead ) final
    {
        return read( -1, outputBuffer, nBytesToRead );
    }

    /* Gzip specific methods */

    /**
     * @return number of processed bits of compressed input file stream.
     * @note It's only useful for a rough estimate because of buffering and because deflate is block based.
     */
    [[nodiscard]] size_t
    tellCompressed() const
    {
        return m_bitReader.tell();
    }

    [[nodiscard]] std::optional<StoppingPoint>
    currentPoint() const
    {
        return m_currentPoint;
    }

    [[nodiscard]] const auto&
    currentDeflateBlock() const
    {
        return m_currentDeflateBlock;
    }

    /**
     * @param[out] outputBuffer should at least be large enough to hold @p nBytesToRead bytes
     * @return number of bytes written
     */
    size_t
    read( const int     outputFileDescriptor = -1,
          char* const   outputBuffer         = nullptr,
          const size_t  nBytesToRead         = std::numeric_limits<size_t>::max(),
          StoppingPoint stoppingPoint        = StoppingPoint::NONE );

    size_t
    read( const WriteFunctor& writeFunctor,
          const size_t        nBytesToRead = std::numeric_limits<size_t>::max(),
          const StoppingPoint stoppingPoint = StoppingPoint::NONE );

    void
    setCRC32Enabled( bool enabled )
    {
        m_crc32Calculator.setEnabled( enabled );
    }

private:
    /**
     * @note Only to be used by readBlock!
     * @return The number of actually flushed bytes, which might be hindered,
     *         e.g., if the output file descriptor can't be written to!
     */
    size_t
    flushOutputBuffer( const WriteFunctor& writeFunctor,
                       size_t              maxBytesToFlush  );

    void
    readBlockHeader();

    /**
     * Decodes data from @ref m_currentDeflateBlock and writes it to the file descriptor and/or the output buffer.
     * It will either return when the whole block has been read or when the requested amount of bytes has been read.
     */
    [[nodiscard]] size_t
    readBlock( const WriteFunctor& writeFunctor,
               size_t              nMaxBytesToDecode );

    void
    readGzipHeader();

    void
    readGzipFooter();

    [[nodiscard]] bool
    bufferHasBeenFlushed() const
    {
        return !m_offsetInLastBuffers.has_value();
    }

    [[nodiscard]] bool
    endOfStream() const
    {
        return !m_currentDeflateBlock.has_value() || !m_currentDeflateBlock->isValid()
               || ( bufferHasBeenFlushed() && m_currentDeflateBlock->eos() );
    }

private:
    pragzip::BitReader m_bitReader;

    size_t m_currentPosition{ 0 }; /** the current position as can only be modified with read or seek calls. */
    bool m_atEndOfFile{ false };

    pragzip::gzip::Header m_lastGzipHeader;
    /**
     * The deflate block will be reused during a gzip stream because each block depends on the last output
     * of the previous block. But after the gzip stream end, this optional will be cleared and in case of
     * another concatenated gzip stream, it will be created anew.
     */
    std::optional<DeflateBlock> m_currentDeflateBlock;
    /** Holds non-owning views to the data decoded in the last call to m_currentDeflateBlock.read. */
    deflate::DecodedDataView m_lastBlockData;

    /**
     * If m_currentPoint has no value, then it means it is currently inside a deflate block.
     * Because a gzip file can contain multiple streams, the file beginning can generically be treated
     * as being at the end of a previous (empty) stream.
     * m_currentPoint may only every have exactly one StoppingPoint set, it may not contain or'ed values!
     */
    std::optional<StoppingPoint> m_currentPoint{ END_OF_STREAM };

    size_t m_streamBytesCount{ 0 };

    /* These are necessary states to return partial results and resume returning further ones.
     * I.e., things which would not be necessary with coroutines supports. This optional has no value
     * iff there is no current deflate block or if we have read all data from it already. */
    std::optional<size_t> m_offsetInLastBuffers;

    CRC32Calculator m_crc32Calculator;
};


[[nodiscard]] std::string
toString( StoppingPoint stoppingPoint );
}  // namespace pragzip
