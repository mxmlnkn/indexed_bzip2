#include "FileUtils.hpp"

#include <cassert>
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

#ifdef _MSC_VER
    #define NOMINMAX
    #include <Windows.h>

    #include <fcntl.h>  // _O_BINARY
    #include <stdio.h>  // stdout
    #include <io.h>     // _setmode
#else
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


#ifdef _MSC_VER
[[nodiscard]] bool
stdinHasInput()
{
    const auto handle = GetStdHandle( STD_INPUT_HANDLE );
    DWORD bytesAvailable{ 0 };
    const auto success = PeekNamedPipe( handle, nullptr, 0, nullptr, &bytesAvailable, nullptr );
    return ( success == 0 ) && ( bytesAvailable > 0 );
}


[[nodiscard]] bool
stdoutIsDevNull()
{
    /**
     * @todo Figure this out on Windows in a reasonable readable manner:
     * @see https://stackoverflow.com/a/21070689/2191065
     */
    return false;
}

#else

[[nodiscard]] bool
stdinHasInput()
{
    pollfd fds{};
    fds.fd = STDIN_FILENO;
    fds.events = POLLIN;
    return poll( &fds, 1, /* timeout in ms */ 0 ) == 1;
}


[[nodiscard]] bool
stdoutIsDevNull()
{
    struct stat devNull{};
    struct stat stdOut{};
    return ( fstat( STDOUT_FILENO, &stdOut ) == 0 ) &&
           ( stat( "/dev/null", &devNull ) == 0 ) &&
           S_ISCHR( stdOut.st_mode ) &&  // NOLINT
           ( devNull.st_dev == stdOut.st_dev ) &&
           ( devNull.st_ino == stdOut.st_ino );
}
#endif


unique_file_ptr
throwingOpen( const std::string& filePath,
              const char*        mode )
{
    if ( mode == nullptr ) {
        throw std::invalid_argument( "Mode must be a C-String and not null!" );
    }

    auto file = make_unique_file_ptr( filePath.c_str(), mode );
    if ( file == nullptr ) {
        std::stringstream msg;
        msg << "Opening file '" << filePath << "' with mode '" << mode << "' failed!";
        throw std::invalid_argument( std::move( msg ).str() );
    }

    return file;
}


unique_file_ptr
throwingOpen( int         fileDescriptor,
              const char* mode )
{
    if ( mode == nullptr ) {
        throw std::invalid_argument( "Mode must be a C-String and not null!" );
    }

    auto file = make_unique_file_ptr( fileDescriptor, mode );
    if ( file == nullptr ) {
        std::stringstream msg;
        msg << "Opening file descriptor " << fileDescriptor << " with mode '" << mode << "' failed!";
        throw std::invalid_argument( std::move( msg ).str() );
    }

    return file;
}


[[nodiscard]] std::string
fdFilePath( int fileDescriptor )
{
    std::stringstream filename;
    filename << "/dev/fd/" << fileDescriptor;
    return filename.str();
}


#ifndef __APPLE_CC__  // Missing std::filesytem::path support in wheels
[[nodiscard]] std::string
findParentFolderContaining( const std::string& folder,
                            const std::string& relativeFilePath )
{
    auto parentFolder = std::filesystem::absolute( folder );
    while ( !parentFolder.empty() )
    {
        const auto filePath = parentFolder / relativeFilePath;
        if ( std::filesystem::exists( filePath ) ) {
            return parentFolder.string();
        }

        if ( parentFolder.parent_path() == parentFolder ) {
            break;
        }
        parentFolder = parentFolder.parent_path();
    }

    return {};
}
#endif


#if defined( HAVE_VMSPLICE )

#include <algorithm>


[[nodiscard]] bool
writeAllSpliceUnsafe( [[maybe_unused]] const int         outputFileDescriptor,
                      [[maybe_unused]] const void* const dataToWrite,
                      [[maybe_unused]] const size_t      dataToWriteSize )
{
    ::iovec dataToSplice{};
    /* The const_cast should be safe because vmsplice should not modify the input data. */
    dataToSplice.iov_base = const_cast<void*>( reinterpret_cast<const void*>( dataToWrite ) );
    dataToSplice.iov_len = dataToWriteSize;
    while ( dataToSplice.iov_len > 0 ) {
        const auto nBytesWritten = ::vmsplice( outputFileDescriptor, &dataToSplice, 1, /* flags */ 0 );
        if ( nBytesWritten < 0 ) {
            if ( dataToSplice.iov_len == dataToWriteSize ) {
                return false;
            }
            std::cerr << "error: " << errno << "\n";
            throw std::runtime_error( "Failed to write to pipe" );
        }
        dataToSplice.iov_base = reinterpret_cast<char*>( dataToSplice.iov_base ) + nBytesWritten;
        dataToSplice.iov_len -= nBytesWritten;
    }
    return true;
}


[[nodiscard]] bool
writeAllSpliceUnsafe( [[maybe_unused]] const int                   outputFileDescriptor,
                      [[maybe_unused]] const std::vector<::iovec>& dataToWrite )
{
    for ( size_t i = 0; i < dataToWrite.size(); ) {
        const auto segmentCount = std::min( static_cast<size_t>( IOV_MAX ), dataToWrite.size() - i );
        auto nBytesWritten = ::vmsplice( outputFileDescriptor, &dataToWrite[i], segmentCount, /* flags */ 0 );

        if ( nBytesWritten < 0 ) {
            if ( i == 0 ) {
                return false;
            }

            std::stringstream message;
            message << "Failed to write all bytes because of: " << strerror( errno ) << " (" << errno << ")";
            throw std::runtime_error( std::move( message.str() ) );
        }

        /* Skip over buffers that were written fully. */
        for ( ; ( i < dataToWrite.size() ) && ( dataToWrite[i].iov_len <= static_cast<size_t>( nBytesWritten ) ); ++i ) {
            nBytesWritten -= dataToWrite[i].iov_len;
        }

        /* Write out last partially written buffer if necessary so that we can resume full vectorized writing
         * from the next iovec buffer. */
        if ( ( i < dataToWrite.size() ) && ( nBytesWritten > 0 ) ) {
            const auto& iovBuffer = dataToWrite[i];

            assert( iovBuffer.iov_len > static_cast<size_t>( nBytesWritten ) );
            const auto size = iovBuffer.iov_len - nBytesWritten;

            const auto remainingData = reinterpret_cast<char*>( iovBuffer.iov_base ) + nBytesWritten;
            if ( !writeAllSpliceUnsafe( outputFileDescriptor, remainingData, size ) ) {
                throw std::runtime_error( "Failed to write to pipe subsequently." );
            }
            ++i;
        }
    }

    return true;
}

#endif  // HAVE_VMSPLICE


/**
 * Posix write is not guaranteed to write everything and in fact was encountered to not write more than
 * 0x7ffff000 (2'147'479'552) B. To avoid this, it has to be looped over.
 */
void
writeAllToFd( const int         outputFileDescriptor,
              const void* const dataToWrite,
              const uint64_t    dataToWriteSize )
{
    for ( uint64_t nTotalWritten = 0; nTotalWritten < dataToWriteSize; ) {
        const auto currentBufferPosition =
            reinterpret_cast<const void*>( reinterpret_cast<uintptr_t>( dataToWrite ) + nTotalWritten );

        const auto nBytesToWritePerCall =
            static_cast<unsigned int>(
                std::min( static_cast<uint64_t>( std::numeric_limits<unsigned int>::max() ),
                dataToWriteSize - nTotalWritten ) );

        const auto nBytesWritten = ::write( outputFileDescriptor, currentBufferPosition, nBytesToWritePerCall );
        if ( nBytesWritten <= 0 ) {
            std::stringstream message;
            message << "Unable to write all data to the given file descriptor. Wrote " << nTotalWritten << " out of "
                    << dataToWriteSize << " (" << strerror( errno ) << ").";
            throw std::runtime_error( std::move( message ).str() );
        }
        nTotalWritten += static_cast<uint64_t>( nBytesWritten );
    }
}


#ifdef HAVE_IOVEC
void
pwriteAllToFd( const int         outputFileDescriptor,
               const void* const dataToWrite,
               const uint64_t    dataToWriteSize,
               const uint64_t    fileOffset )
{
    for ( uint64_t nTotalWritten = 0; nTotalWritten < dataToWriteSize; ) {
        const auto currentBufferPosition =
            reinterpret_cast<const void*>( reinterpret_cast<uintptr_t>( dataToWrite ) + nTotalWritten );
        const auto nBytesWritten = ::pwrite( outputFileDescriptor,
                                             currentBufferPosition,
                                             dataToWriteSize - nTotalWritten,
                                             fileOffset + nTotalWritten );
        if ( nBytesWritten <= 0 ) {
            std::stringstream message;
            message << "Unable to write all data to the given file descriptor. Wrote " << nTotalWritten << " out of "
                    << dataToWriteSize << " (" << strerror( errno ) << ").";
            throw std::runtime_error( std::move( message ).str() );
        }

        nTotalWritten += static_cast<uint64_t>( nBytesWritten );
    }
}


void
writeAllToFdVector( const int                   outputFileDescriptor,
                    const std::vector<::iovec>& dataToWrite )
{
    for ( size_t i = 0; i < dataToWrite.size(); ) {
        const auto segmentCount = std::min( static_cast<size_t>( IOV_MAX ), dataToWrite.size() - i );
        auto nBytesWritten = ::writev( outputFileDescriptor, &dataToWrite[i], segmentCount );

        if ( nBytesWritten < 0 ) {
            std::stringstream message;
            message << "Failed to write all bytes because of: " << strerror( errno ) << " (" << errno << ")";
            throw std::runtime_error( std::move( message.str() ) );
        }

        /* Skip over buffers that were written fully. */
        for ( ; ( i < dataToWrite.size() ) && ( dataToWrite[i].iov_len <= static_cast<size_t>( nBytesWritten ) ); ++i ) {
            nBytesWritten -= dataToWrite[i].iov_len;
        }

        /* Write out last partially written buffer if necessary so that we can resume full vectorized writing
         * from the next iovec buffer. */
        if ( ( i < dataToWrite.size() ) && ( nBytesWritten > 0 ) ) {
            const auto& iovBuffer = dataToWrite[i];

            assert( iovBuffer.iov_len < static_cast<size_t>( nBytesWritten ) );
            const auto remainingSize = iovBuffer.iov_len - nBytesWritten;
            const auto remainingData = reinterpret_cast<char*>( iovBuffer.iov_base ) + nBytesWritten;
            writeAllToFd( outputFileDescriptor, remainingData, remainingSize );

            ++i;
        }
    }
}


void
pwriteAllToFdVector( const int                   outputFileDescriptor,
                     const std::vector<::iovec>& dataToWrite,
                     size_t                      fileOffset )
{
    for ( size_t i = 0; i < dataToWrite.size(); ) {
        const auto segmentCount = std::min( static_cast<size_t>( IOV_MAX ), dataToWrite.size() - i );
        auto nBytesWritten = ::pwritev( outputFileDescriptor, &dataToWrite[i], segmentCount, fileOffset );

        if ( nBytesWritten < 0 ) {
            std::stringstream message;
            message << "Failed to write all bytes because of: " << strerror( errno ) << " (" << errno << ")";
            throw std::runtime_error( std::move( message.str() ) );
        }

        fileOffset += nBytesWritten;

        /* Skip over buffers that were written fully. */
        for ( ; ( i < dataToWrite.size() ) && ( dataToWrite[i].iov_len <= static_cast<size_t>( nBytesWritten ) ); ++i ) {
            nBytesWritten -= dataToWrite[i].iov_len;
        }

        /* Write out last partially written buffer if necessary so that we can resume full vectorized writing
         * from the next iovec buffer. */
        if ( ( i < dataToWrite.size() ) && ( nBytesWritten > 0 ) ) {
            const auto& iovBuffer = dataToWrite[i];

            assert( iovBuffer.iov_len < static_cast<size_t>( nBytesWritten ) );
            const auto remainingSize = iovBuffer.iov_len - nBytesWritten;
            const auto remainingData = reinterpret_cast<char*>( iovBuffer.iov_base ) + nBytesWritten;
            pwriteAllToFd( outputFileDescriptor, remainingData, remainingSize, fileOffset );
            fileOffset += remainingSize;

            ++i;
        }
    }
}
#endif  // HAVE_IOVEC


void
writeAll( const int         outputFileDescriptor,
          void* const       outputBuffer,
          const void* const dataToWrite,
          const uint64_t    dataToWriteSize )
{
    if ( dataToWriteSize == 0 ) {
        return;
    }

    if ( outputFileDescriptor >= 0 ) {
        writeAllToFd( outputFileDescriptor, dataToWrite, dataToWriteSize );
    }

    if ( outputBuffer != nullptr ) {
        if ( dataToWriteSize > std::numeric_limits<size_t>::max() ) {
            throw std::invalid_argument( "Too much data to write!" );
        }
        std::memcpy( outputBuffer, dataToWrite, dataToWriteSize );
    }
}


OutputFile::OutputFile( const std::string& filePath ) :
    m_writingToStdout( filePath.empty() )
{
    if ( filePath.empty() ) {
    #ifdef _MSC_VER
        m_fileDescriptor = _fileno( stdout );
        _setmode( m_fileDescriptor, _O_BINARY );
    #else
        m_fileDescriptor = ::fileno( stdout );
    #endif
    } else {
    #ifndef _MSC_VER
        if ( fileExists( filePath ) ) {
            m_oldOutputFileSize = fileSize( filePath );
            /* Opening an existing file and overwriting its data can be much slower because posix_fallocate
             * can be relatively slow compared to the decoding speed and memory bandwidth! Note that std::fopen
             * would open a file with O_TRUNC, deallocating all its contents before it has to be reallocated. */
            m_fileDescriptor = ::open( filePath.c_str(), O_WRONLY );
            m_ownedFd = unique_file_descriptor( m_fileDescriptor );
        }
    #endif

        if ( m_fileDescriptor == -1 ) {
            m_outputFile = make_unique_file_ptr( filePath.c_str(), "wb" );
            if ( !m_outputFile ) {
                std::cerr << "Could not open output file: " << filePath << " for writing!\n";
                throw std::runtime_error( "File could not be opened." );
            }
            m_fileDescriptor = ::fileno( m_outputFile.get() );
        }
    }
}

void
OutputFile::truncate( size_t size )
{
#ifndef _MSC_VER
    if ( ( m_fileDescriptor != -1 ) && ( size < m_oldOutputFileSize ) ) {
        if ( ::ftruncate( m_fileDescriptor, size ) == -1 ) {
            std::cerr << "[Error] Failed to truncate file because of: " << strerror( errno )
                      << " (" << errno << ")\n";
        }
    }
#endif
}
