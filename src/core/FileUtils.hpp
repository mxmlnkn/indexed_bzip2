#pragma once

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <sys/stat.h>

#ifndef _MSC_VER
    #ifndef _GNU_SOURCE
        #define _GNU_SOURCE
    #endif

    #include <errno.h>
    #include <fcntl.h>
    #include <limits.h>         // IOV_MAX
    #include <sys/stat.h>
    #include <sys/poll.h>
    #include <sys/uio.h>
    #include <unistd.h>

    #if not defined( HAVE_VMSPLICE ) and defined( __linux__ )
        #define HAVE_VMSPLICE
    #endif

    #if not defined( HAVE_IOVEC ) and defined( __linux__ )
        #define HAVE_IOVEC
    #endif
#endif


#if defined( HAVE_VMSPLICE )
    #include <any>
    #include <deque>

    #include <AtomicMutex.hpp>
#endif


[[nodiscard]] bool
stdinHasInput();


[[nodiscard]] bool
stdoutIsDevNull();


inline bool
fileExists( const std::string& filePath )
{
    return std::ifstream( filePath ).good();
}


inline size_t
fileSize( const std::string& filePath )
{
    std::ifstream file( filePath );
    file.seekg( 0, std::ios_base::end );
    const auto result = file.tellg();
    if ( result < 0 ) {
        throw std::invalid_argument( "Could not get size of specified file!" );
    }
    return static_cast<size_t>( result );
}


inline size_t
filePosition( std::FILE* file )
{
    const auto offset = std::ftell( file );
    if ( offset < 0 ) {
        throw std::runtime_error( "Could not get the file position!" );
    }
    return static_cast<size_t>( offset );
}


#ifndef _MSC_VER
struct unique_file_descriptor
{
    explicit
    unique_file_descriptor( int fd ) :
        m_fd( fd )
    {}

    ~unique_file_descriptor()
    {
        close();
    }

    unique_file_descriptor() = default;
    unique_file_descriptor( const unique_file_descriptor& ) = delete;
    unique_file_descriptor& operator=( const unique_file_descriptor& ) = delete;

    unique_file_descriptor( unique_file_descriptor&& other ) noexcept :
        m_fd( other.m_fd )
    {
        other.m_fd = -1;
    }

    unique_file_descriptor&
    operator=( unique_file_descriptor&& other ) noexcept
    {
        close();
        m_fd = other.m_fd;
        other.m_fd = -1;
        return *this;
    }

    [[nodiscard]] constexpr int
    operator*() const noexcept
    {
        return m_fd;
    }

    void
    close()
    {
        if ( m_fd >= 0 ) {
            ::close( m_fd );
        }
    }

    void
    release()
    {
        m_fd = -1;
    }

private:
    int m_fd{ -1 };
};
#endif  // ifndef _MSC_VER


using unique_file_ptr = std::unique_ptr<std::FILE, std::function<void ( std::FILE* )> >;

inline unique_file_ptr
make_unique_file_ptr( std::FILE* file )
{
    return unique_file_ptr( file, []( auto* ownedFile ){
        if ( ownedFile != nullptr ) {
            std::fclose( ownedFile );
        } } );
}

inline unique_file_ptr
make_unique_file_ptr( char const* const filePath,
                      char const* const mode )
{
    return make_unique_file_ptr( std::fopen( filePath, mode ) );
}

inline unique_file_ptr
make_unique_file_ptr( int         fileDescriptor,
                      char const* mode )
{
    return make_unique_file_ptr( fdopen( fileDescriptor, mode ) );
}



unique_file_ptr
throwingOpen( const std::string& filePath,
              const char*        mode );


unique_file_ptr
throwingOpen( int         fileDescriptor,
              const char* mode );


/** dup is not strong enough to be able to independently seek in the old and the dup'ed fd! */
[[nodiscard]] std::string
fdFilePath( int fileDescriptor );


#ifndef __APPLE_CC__  // Missing std::filesytem::path support in wheels
[[nodiscard]] inline std::string
findParentFolderContaining( const std::string& folder,
                            const std::string& relativeFilePath );
#endif


#if defined( HAVE_VMSPLICE )

#include <algorithm>

/**
 * Short overview of syscalls that optimize copies by instead copying full page pointers into the
 * pipe buffers inside the kernel:
 * - splice: <fd (pipe or not)> <-> <pipe>
 * - vmsplice: memory -> <pipe>
 * - mmap: <fd> -> memory
 * - sendfile: <fd that supports mmap> -> <fd (before Linux 2.6.33 (2010-02-24) it had to be a socket fd)>
 *
 * I think the underlying problem with wrong output data for small chunk sizes
 * is that vmsplice is not as "synchronous" as I thought it to be:
 *
 * https://lwn.net/Articles/181169/
 *
 *  - Determining whether it is safe to write to a vmspliced buffer is
 *    suggested to be done implicitly by splicing more than the maximum
 *    number of pages that can be inserted into the pipe buffer.
 *    That number was supposed to be queryable with fcntl F_GETPSZ.
 *    -> This is probably why I didn't notice problems with larger chunk
 *       sizes.
 *  - Even that might not be safe enough when there are multiple pipe
 *    buffers.
 *
 * https://stackoverflow.com/questions/70515745/how-do-i-use-vmsplice-to-correctly-output-to-a-pipe
 * https://codegolf.stackexchange.com/questions/215216/high-throughput-fizz-buzz/239848#239848
 *
 *  - the safest way to use vmsplice seems to be mmap -> vmplice with
 *    SPLICE_F_GIFT -> munmap. munmap can be called directly after the
 *    return from vmplice and this works in a similar way to aio_write
 *    but actually a lot faster.
 *
 * I think using std::vector with vmsplice is NOT safe when it is
 * destructed too soon! The problem here is that the memory is probably not
 * returned to the system, which would be fine, but is actually reused by
 * the c/C++ standard library's implementation of malloc/free/new/delete:
 *
 * https://stackoverflow.com/a/1119334
 *
 *  - In many malloc/free implementations, free does normally not return
 *    the memory to the operating system (or at least only in rare cases).
 *    [...] Free will put the memory block in its own free block list.
 *    Normally it also tries to meld together adjacent blocks in the
 *    address space.
 *
 * https://mazzo.li/posts/fast-pipes.html
 * https://github.com/bitonic/pipes-speed-test.git
 *
 *  - Set pipe size and double buffer. (Similar to the lwn article
 *    but instead of querying the pipe size, it is set.)
 *  - fcntl(STDOUT_FILENO, F_SETPIPE_SZ, options.pipe_size);
 *
 * I think I will have to implement a container with a custom allocator
 * that uses mmap and munmap to get back my vmsplice speeds :/(.
 * Or maybe try setting the pipe buffer size to some forced value and
 * then only free the last data after pipe size more has been written.
 *
 * @note Throws if some splice calls were successful followed by an unsucessful one before finishing.
 * @return true if successful and false if it could not be spliced from the beginning, e.g., because the file
 *         descriptor is not a pipe.
 */
[[nodiscard]] inline bool
writeAllSpliceUnsafe( [[maybe_unused]] const int         outputFileDescriptor,
                      [[maybe_unused]] const void* const dataToWrite,
                      [[maybe_unused]] const size_t      dataToWriteSize );


[[nodiscard]] inline bool
writeAllSpliceUnsafe( [[maybe_unused]] const int                   outputFileDescriptor,
                      [[maybe_unused]] const std::vector<::iovec>& dataToWrite );


/**
 * Keeps shared pointers to spliced objects until an amount of bytes equal to the pipe buffer size
 * has been spliced into the pipe.
 * It implements a singleton-like (singleton per file descriptor) interface as a performance optimization.
 * Without a global ledger, the effectively held back objects would be overestimated by the number of actual ledgers.
 */
class SpliceVault
{
public:
    using VaultLock = std::unique_lock<AtomicMutex>;

public:
    [[nodiscard]] static std::pair<SpliceVault*, VaultLock>
    getInstance( int fileDescriptor )
    {
        static AtomicMutex mutex;
        static std::unordered_map<int, std::unique_ptr<SpliceVault> > vaults;

        const std::scoped_lock lock{ mutex };
        auto vault = vaults.find( fileDescriptor );
        if ( vault == vaults.end() ) {
            /* try_emplace cannot be used because the SpliceVault constructor is private. */
            vault = vaults.emplace( fileDescriptor,
                                    std::unique_ptr<SpliceVault>( new SpliceVault( fileDescriptor ) ) ).first;
        }
        return std::make_pair( vault->second.get(), vault->second->lock() );
    }

    /**
     * @param dataToWrite A pointer to the start of the data to write. This pointer should be part of @p splicedData!
     * @param splicedData This owning shared pointer will be stored until enough other data has been spliced into
     *                    the pipe.
     */
    template<typename T>
    [[nodiscard]] bool
    splice( const void* const         dataToWrite,
            size_t const              dataToWriteSize,
            const std::shared_ptr<T>& splicedData )
    {
        if ( ( m_pipeBufferSize < 0 )
             || !writeAllSpliceUnsafe( m_fileDescriptor, dataToWrite, dataToWriteSize ) ) {
            return false;
        }

        account( splicedData, dataToWriteSize );
        return true;
    }

    /**
     * Overload that works for iovec structures directly.
     */
    template<typename T>
    [[nodiscard]] bool
    splice( const std::vector<::iovec>& buffersToWrite,
            const std::shared_ptr<T>&   splicedData )
    {
        if ( ( m_pipeBufferSize < 0 )
             || !writeAllSpliceUnsafe( m_fileDescriptor, buffersToWrite ) ) {
            return false;
        }

        const auto dataToWriteSize = std::accumulate(
            buffersToWrite.begin(), buffersToWrite.end(), size_t( 0 ),
            [] ( size_t sum, const auto& buffer ) { return sum + buffer.iov_len; } );

        account( splicedData, dataToWriteSize );
        return true;
    }


private:
    template<typename T>
    void
    account( const std::shared_ptr<T>& splicedData,
             size_t const              dataToWriteSize )
    {
        m_totalSplicedBytes += dataToWriteSize;
        /* Append written size to last shared pointer if it is the same one or add a new data set. */
        if ( !m_splicedData.empty() && ( std::get<1>( m_splicedData.back() ) == splicedData.get() ) ) {
            std::get<2>( m_splicedData.back() ) += dataToWriteSize;
        } else {
            m_splicedData.emplace_back( splicedData, splicedData.get(), dataToWriteSize );
        }

        /* Never fully clear the shared pointers even if the size of the last is larger than the pipe buffer
         * because part of that last large chunk will still be in the pipe buffer! */
        while ( !m_splicedData.empty()
                && ( m_totalSplicedBytes - std::get<2>( m_splicedData.front() )
                     >= static_cast<size_t>( m_pipeBufferSize ) ) ) {
            m_totalSplicedBytes -= std::get<2>( m_splicedData.front() );
            m_splicedData.pop_front();
        }
    }

    explicit
    SpliceVault( int fileDescriptor ) :
        m_fileDescriptor( fileDescriptor ),
        m_pipeBufferSize( fcntl( fileDescriptor, F_GETPIPE_SZ ) )
    {}

    [[nodiscard]] VaultLock
    lock()
    {
        return VaultLock( m_mutex );
    }

private:
    const int m_fileDescriptor;
    /** We are assuming here that the pipe buffer size does not change to avoid frequent calls to fcntl. */
    const int m_pipeBufferSize;

    /**
     * Contains shared_ptr to extend lifetime and amount of bytes that have been spliced to determine
     * when the shared_ptr can be removed from this list.
     */
    std::deque<std::tuple</* packed RAII resource */ std::any,
                          /* raw pointer of RAII resource for comparison */ const void*,
                          /* spliced bytes */ size_t> > m_splicedData;
    /**
     * This data is redundant but helps to avoid O(N) recalculation of this value from @ref m_splicedData.
     */
    size_t m_totalSplicedBytes{ 0 };

    AtomicMutex m_mutex;
};
#endif  // HAVE_VMSPLICE


/**
 * Posix write is not guaranteed to write everything and in fact was encountered to not write more than
 * 0x7ffff000 (2'147'479'552) B. To avoid this, it has to be looped over.
 */
inline void
writeAllToFd( const int         outputFileDescriptor,
              const void* const dataToWrite,
              const uint64_t    dataToWriteSize );


#ifdef HAVE_IOVEC
inline void
pwriteAllToFd( const int         outputFileDescriptor,
               const void* const dataToWrite,
               const uint64_t    dataToWriteSize,
               const uint64_t    fileOffset );


inline void
writeAllToFdVector( const int                   outputFileDescriptor,
                    const std::vector<::iovec>& dataToWrite );


inline void
pwriteAllToFdVector( const int                   outputFileDescriptor,
                     const std::vector<::iovec>& dataToWrite,
                     size_t                      fileOffset );
#endif  // HAVE_IOVEC


inline void
writeAll( const int         outputFileDescriptor,
          void* const       outputBuffer,
          const void* const dataToWrite,
          const uint64_t    dataToWriteSize );


/**
 * Wrapper to open either stdout, a given existing file without truncation for better performance, or a new file.
 */
class OutputFile
{
public:
    explicit
    OutputFile( const std::string& filePath );

    void
    truncate( size_t size );

    [[nodiscard]] bool
    writingToStdout() const noexcept
    {
        return m_writingToStdout;
    }

    [[nodiscard]] int
    fd() const noexcept
    {
        return m_fileDescriptor;
    }

private:
    const bool m_writingToStdout;

    int m_fileDescriptor{ -1 };  // Use this for file access.

    /** This is used to decide whether to truncate the file to a smaller (decompressed) size before closing. */
    size_t m_oldOutputFileSize{ 0 };

    /**
     * These should not be used. They are only for automatic closing!
     * Two of them because a file may either be opened from an existing file without truncation,
     * which does not fit into unique_file_ptr, or it might be newly created.
     */
    unique_file_ptr m_outputFile;
#ifndef _MSC_VER
    unique_file_descriptor m_ownedFd;  // This should not be used, it is only for automatic closing!
#endif
};
