#pragma once

#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <common.hpp>
#include <FasterVector.hpp>
#include <JoiningThread.hpp>

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
    {
        /* ~1.75s */
        //const auto t0 = now();
        //bufferUpTo( std::numeric_limits<size_t>::max() );
        //const auto t1 = now();
        //std::cerr << "Buffering the whole file took: " << duration( t0, t1 ) << " s\n";
    }

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
        m_cancelReaderThread = true;
        m_notifyReaderThread.notify_one();
        m_readerThread.reset();
        /* No locking necessary because the reader thread is joined. */
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
        /** @todo need to lock m_bufferMutex !!!*/
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
        return m_underlyingFileEOF ? m_numberOfBytesRead.load() : 0;
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
            const std::lock_guard lock( m_fileMutex );
            m_file->clearerr();
        }
    }

private:
    void
    bufferUpTo( const size_t untilOffset )
    {
        if ( m_underlyingFileEOF || ( untilOffset <= m_bufferUntilOffset ) ) {
            return;
        }

        m_bufferUntilOffset = untilOffset;
        //std::cerr << "Set m_bufferUntilOffset: " << m_bufferUntilOffset << "\n";
        m_notifyReaderThread.notify_one();

        std::unique_lock lock( m_bufferMutex );
        m_bufferChanged.wait( lock, [this, untilOffset] () {
            //std::cerr << "[bufferUpTo] m_underlyingFileEOF: " << m_underlyingFileEOF << ", m_buffer.size(): " << m_buffer.size() << "\n";
            return m_underlyingFileEOF || ( m_buffer.size () * CHUNK_SIZE >= untilOffset );
        } );
    }

    void
    readerThreadMain()
    {
        /* The smart pointer m_file must never change while this thread runs!
         * The FileReader object it points to might change, e.g., via clearerr. */
        if ( !m_file ) {
            return;
        }

        while ( !m_cancelReaderThread && !m_underlyingFileEOF ) {
            if ( m_numberOfBytesRead >= saturatingAddition( m_bufferUntilOffset.load(), 64 * CHUNK_SIZE ) ) {
                std::unique_lock lock( m_bufferUntilOffsetMutex );
                m_notifyReaderThread.wait( lock, [this] () {
                    return m_cancelReaderThread || ( m_numberOfBytesRead < saturatingAddition( m_bufferUntilOffset.load(), 64 * CHUNK_SIZE ) );
                } );
                continue;
            }

            FasterVector<std::byte> chunk( CHUNK_SIZE );
            const auto nBytesRead = m_file->read( reinterpret_cast<char*>( chunk.data() ), chunk.size() );
            chunk.resize( nBytesRead );

            {
                std::lock_guard lock( m_bufferMutex );
                m_numberOfBytesRead += nBytesRead;
                m_underlyingFileEOF = nBytesRead == 0;
                if ( m_underlyingFileEOF ) {
                    //std::cerr << "Set m_underlyingFileEOF: " << m_underlyingFileEOF << "\n";
                }
                m_buffer.emplace_back( std::move( chunk ) );
                //m_buffer.clear();
            }
            m_bufferChanged.notify_all();
        }

        if ( m_underlyingFileEOF ) {
            //std::cerr << "Finished buffering the whole file!\n";
        }
    }

protected:
    UniqueFileReader m_file;
    std::mutex m_fileMutex;

    size_t m_currentPosition{ 0 };

    /** Ensures that up to offset is buffered. Might also buffer more. May only increase. */
    std::atomic<size_t> m_bufferUntilOffset{ 0 };
    std::mutex m_bufferUntilOffsetMutex;

    /** These are only modified by @ref m_readerThread. */
    std::atomic<bool> m_underlyingFileEOF{ false };
    std::atomic<size_t> m_numberOfBytesRead{ 0 };

    std::deque<FasterVector<std::byte> > m_buffer;
    std::mutex m_bufferMutex;
    std::condition_variable m_bufferChanged;

    std::atomic<bool> m_cancelReaderThread{ false };

    /** Signaled on m_bufferUntilOffset and also m_cancelReaderThread changes. */
    std::condition_variable m_notifyReaderThread;

    /** Fills m_buffer on demand. */
    std::unique_ptr<JoiningThread> m_readerThread{
        std::make_unique<JoiningThread>( [this] () { readerThreadMain(); } )
    };
};


/*
Communication protocol between the SinglePass interface called
from one thread (not reentrant!) and the reader thread:

        SinglePass                  readerThreadMain
            |                              |
            |---------- creates ---------->|
            |                              |
            |                              | wait for m_untilOffset change
            |                              |
            |--- increment m_untilOffset ->|
            |                              | buffer block-wise
            |                              |   1. unlock during reading
            |                              |   2. lock during appending to the deque
            |                              |   3. signal after each append
            |                              |      so that the requester can resume ASAP
            |                              | wait for m_untilOffset change
            |                              |
            |--- increment m_untilOffset ->|
            |                              |


Before dedicated reader thread:

    [21:40:47.412][139636660877184] [SharedFileReader::~SharedFileReader]
        seeks back    : ( 0 <= 34000000 +- 23000000 <= 135000000  ) B ( 12810 calls )
        seeks forward : ( 0 <= 33000000 +- 23000000 <= 151000000  ) B ( 12875 calls )
        reads         : ( 0 <= 131000 +- 1900 <= 131100  ) B ( 25686 calls )
        locks         : 26468
        read in total 3366011301 B out of 0 B, i.e., read the file 0 times
        time spent seeking and reading: 44.2804 s

    Decompressed in total 4294967296 B in 3.06991 s -> 1399.05 MB/s

After adding dedicated reader thread:

    [23:07:33.617][140647918958464] [SharedFileReader::~SharedFileReader]
        seeks back    : ( 0 <= 33000000 +- 22000000 <= 134000000  ) B ( 12866 calls )
        seeks forward : ( 3000000 <= 33000000 +- 22000000 <= 126000000  ) B ( 12852 calls )
        reads         : ( 0 <= 131100 +- 1200 <= 131100  ) B ( 25722 calls )
        locks         : 26547
        read in total 3371106725 B out of 0 B, i.e., read the file 0 times
        time spent seeking and reading: 33.4876 s

    Decompressed in total 4294967296 B in 2.51169 s -> 1709.99 MB/s


 :/ fuck... something is not right.

Buffering the whole file in the constructor:

    Buffering the whole file took: 1.96587 s
    [ParallelGzipReader] Time spent:
        Writing to output         : 0.00563254 s
        Computing CRC32           : 7.4811e-05 s
        Number of verified CRC32s : 0
    [GzipChunkFetcher::GzipChunkFetcher] First block access statistics:
        Time spent in block finder          : 0.394926 s
        Time spent decoding                 : 26.6046 s
        Time spent allocating and copying   : 0.699966 s
        Time spent applying the last window : 0.0425921 s
        Replaced marker bytes               : 21 MiB 582 KiB 252 B
    [...]
    [23:15:22.690][140273455040384] [SharedFileReader::~SharedFileReader]
        seeks back    : ( 0 <= 34000000 +- 23000000 <= 143000000  ) B ( 13066 calls )
        seeks forward : ( 2000000 <= 34000000 +- 23000000 <= 147000000  ) B ( 13091 calls )
        reads         : ( 0 <= 131100 +- 1200 <= 131100  ) B ( 26159 calls )
        locks         : 27508
        read in total 3428385189 B out of 3263906195 B, i.e., read the file 1.05039 times
        time spent seeking and reading: 0.604416 s

    Decompressed in total 4294967296 B in 3.13335 s -> 1370.73 MB/s

 -> "time spent seeking and reading" looks ok here so why doesn't it work to overlap buffering and decompression :/?!
    There is the very first batch, which might take a while I guess. With a chunk size of 4 MiB, I need to buffer
    Buffering the whole file took 2s in this run! This is almost as long as the whole decompression took in the
    run above! I think the problem might be the reading from pipe!
        -> Try to use fcat or something like that? And maybe I can use unbuffered read inside SinglePass?

time cat 4GiB-base64.gz | wc -c  # ~ 1s! Twice as fast as we are! Then again, rpmalloc might take most of the time

When only keeping one chunk to reduce memory pressure and the amount of raw allocations:

    Buffering the whole file took: 1.86504 s

After disabling std::cerr inside bufferUpTo condition_variable wait:

    Buffering the whole file took: 1.87034 s

Using setvbuf( f, (char *)NULL, _IONBF, 0 );

    Buffering the whole file took: 1.60156 s

cat | instead of <( cat ):

    Buffering the whole file took: 1.6215 s

std::fread( buffer, nMaxBytesToRead, 1, m_file.get() );
instead of
std::fread( buffer, 1, nMaxBytesToRead, m_file.get() );

    Buffering the whole file took: 1.59095 s

::read instead of std::fread

    Buffering the whole file took: 1.58627 s


Decrease CHUNK_SIZE from 4_Mi to 1_Mi

    Buffering the whole file took: 1.10116 s

Puh ... why? Because it stays in cache in my simple example where I only keep one chunk?
Or worse because of some bucketing issue / worst-case with rpmalloc?

Let it keep all chunks again instead of only the last one:

    Buffering the whole file took: 1.83047 s

 - [ ] Ok, then the next step for now is to implement the release mechanism to avoid buffering everything

Memory consumption:

/usr/bin/time -v src/tools/pragzip -v -P 0 -d -o /dev/null <( cat 4GiB-base64.gz )

    Buffering the whole file took: 1.78626 s
    Maximum resident set size (kbytes): 3515440

CHUNK_SIZE 4_Mi

    Buffering the whole file took: 1.91364 s
	Maximum resident set size (kbytes): 3529048

    -> at least not an overallocation issue. Looks more like a caching issue

Without buffering everything inside the constructor:

    [00:05:59.179][139705274423168] [SharedFileReader::~SharedFileReader]
        seeks back    : ( 0 <= 33000000 +- 23000000 <= 122000000  ) B ( 12791 calls )
        seeks forward : ( 0 <= 33000000 +- 23000000 <= 126000000  ) B ( 12897 calls )
        reads         : ( 0 <= 131000 +- 3000 <= 131000  ) B ( 25699 calls )
        locks         : 26494
        read in total 3366011301 B out of 0 B, i.e., read the file 0 times
        time spent seeking and reading: 32.5572 s

    Decompressed in total 4294967296 B in 2.53459 s -> 1694.54 MB/s

  -> Same as with CHUNK_SIZE 4_Mi and disabling the buffering also doesn't do aynthing.
     I guess the last ~1 s that can be improved is the memory reuse / release and/or the locks on SharedFileReader.

Without pipe: /usr/bin/time -v src/tools/pragzip -v -P 0 -d -o /dev/null 4GiB-base64.gz

    [00:08:08.755][140423888447360] [SharedFileReader::~SharedFileReader]
        seeks back    : ( 0 <= 34000000 +- 23000000 <= 164000000  ) B ( 13039 calls )
        seeks forward : ( 0 <= 34000000 +- 23000000 <= 156000000  ) B ( 13118 calls )
        reads         : ( 0 <= 131100 +- 1200 <= 131100  ) B ( 26159 calls )
        locks         : 1349
        read in total 3428385189 B out of 3263906195 B, i.e., read the file 1.05039 times
        time spent seeking and reading: 0.600941 s

    Decompressed in total 4294967296 B in 1.16197 s -> 3696.28 MB/s

    Maximum resident set size (kbytes): 306480

 -> Many fewer locks in SharedFileReader! and only 300 MB instead of 3.5 GB maximum RSS
*/
