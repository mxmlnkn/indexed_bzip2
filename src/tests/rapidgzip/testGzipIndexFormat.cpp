#include <iostream>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include <common.hpp>
#include <IndexFileFormat.hpp>
#include <filereader/Standard.hpp>
#include <TestHelpers.hpp>


GzipIndex
testIndexRead( const std::filesystem::path& compressedPath,
               const std::filesystem::path& uncompressedPath,
               const std::filesystem::path& indexPath )
{
    auto index = readGzipIndex( std::make_unique<StandardFileReader>( indexPath ) );

    REQUIRE_EQUAL( index.compressedSizeInBytes, fileSize( compressedPath ) );
    REQUIRE_EQUAL( index.uncompressedSizeInBytes, fileSize( uncompressedPath ) );

    REQUIRE_EQUAL( index.checkpointSpacing, 64_Ki );
    REQUIRE_EQUAL( index.checkpoints.size(), 5U );

    REQUIRE( static_cast<bool>( index.windows ) );

    return index;
}


void
testIndexReadWrite( const std::filesystem::path& compressedPath,
                    const std::filesystem::path& uncompressedPath,
                    const std::filesystem::path& indexPath )
{
    const auto index = testIndexRead( compressedPath, uncompressedPath, indexPath );

    try
    {
        const auto tmpFolder = createTemporaryDirectory( "rapidgzip.testGzipIndexFormat" );
        const auto gzipIndexPath = tmpFolder.path() / "gzipindex";

        {
            const auto file = throwingOpen( gzipIndexPath, "wb" );
            const auto checkedWrite =
                [&file] ( const void* buffer, size_t size )
                {
                    if ( std::fwrite( buffer, 1, size, file.get() ) != size ) {
                        throw std::runtime_error( "Failed to write data to index!" );
                    }
                };
            writeGzipIndex( index, checkedWrite );
        }
        const auto rereadIndex = readGzipIndex( std::make_unique<StandardFileReader>( gzipIndexPath ) );

        REQUIRE_EQUAL( rereadIndex.compressedSizeInBytes, index.compressedSizeInBytes );
        REQUIRE_EQUAL( rereadIndex.uncompressedSizeInBytes, index.uncompressedSizeInBytes );
        REQUIRE_EQUAL( rereadIndex.checkpointSpacing, index.checkpointSpacing );
        REQUIRE_EQUAL( rereadIndex.windowSizeInBytes, index.windowSizeInBytes );

        REQUIRE( rereadIndex.checkpoints == index.checkpoints );

        REQUIRE_EQUAL( static_cast<bool>( rereadIndex.windows ), static_cast<bool>( index.windows ) );
        if ( rereadIndex.windows && index.windows ) {
            REQUIRE_EQUAL( rereadIndex.windows->size(), index.windows->size() );
            const auto& [_, windows] = index.windows->data();
            for ( const auto& [offset, window] : *windows ) {
                const auto rereadWindow = rereadIndex.windows->get( offset );
                if ( !rereadWindow ) {
                    std::cerr << "Failed to find offset " << offset << " in reread index!\n";
                    continue;
                }

                if ( static_cast<bool>( window ) != static_cast<bool>( rereadWindow ) ) {
                    std::stringstream message;
                    message << std::boolalpha << "Shared window has value: " << static_cast<bool>( window )
                            << " while reread shared window has value: " << static_cast<bool>( rereadWindow );
                    std::cerr << std::move( message ).str() << "\n";
                    continue;
                }

                if ( *window != *rereadWindow ) {
                    std::cerr << "Window contents for offset " << offset << " differ!\n";
                    continue;
                }
            }
        }

        REQUIRE( rereadIndex == index );
    }
    catch ( const std::exception& exception )
    {
        /* Note that the destructor for TemporaryDirectory might not be called for uncaught exceptions!
         * @see https://stackoverflow.com/questions/222175/why-destructor-is-not-called-on-exception */
        std::cerr << "Caught exception: " << exception.what() << "\n";
        REQUIRE( false );
    }
}


GzipIndex
testBzipIndexRead( const std::filesystem::path& compressedPath,
                   const std::filesystem::path& uncompressedPath,
                   const std::filesystem::path& indexPath )
{
    auto index = readGzipIndex( std::make_unique<StandardFileReader>( indexPath ),
                                /* This second argument is only necessary when reading bgzip indexes! */
                                std::make_unique<StandardFileReader>( compressedPath ) );

    REQUIRE_EQUAL( index.compressedSizeInBytes, fileSize( compressedPath ) );
    REQUIRE_EQUAL( index.uncompressedSizeInBytes, fileSize( uncompressedPath ) );

    /* checkpointSpacing is not available for bgzip indexes. */
    REQUIRE_EQUAL( index.checkpointSpacing, 0U );
    REQUIRE_EQUAL( index.checkpoints.size(), 4U );

    REQUIRE( static_cast<bool>( index.windows ) );

    return index;
}


void
testRandomAccessIndex()
{
    std::vector<char> buffer;
    const auto writeToBuffer =
        [&buffer] ( const void* const data, const size_t size )
        {
            const auto oldSize = buffer.size();
            buffer.resize( oldSize + size );
            std::memcpy( buffer.data() + oldSize, data, size );
        };

    GzipIndex index;
    index.compressedSizeInBytes = 1_Mi;
    index.windows = std::make_shared<WindowMap>();

    {
        auto& checkpoint = index.checkpoints.emplace_back();
        checkpoint.compressedOffsetInBits = 0;
        checkpoint.uncompressedOffsetInBytes = 20;

        index.windows->emplace( checkpoint.compressedOffsetInBits, {} );
    }

    {
        auto& checkpoint = index.checkpoints.emplace_back();
        checkpoint.compressedOffsetInBits = 100;
        checkpoint.uncompressedOffsetInBytes = 40;

        const std::vector<char> window = { 1, 2, 3 };
        index.windows->emplace( checkpoint.compressedOffsetInBits, window );
    }

    RandomAccessIndex::writeGzipIndex( index, writeToBuffer );

    const auto restoredIndex = RandomAccessIndex::readGzipIndex( std::make_unique<BufferViewFileReader>( buffer ),
                                                                 index.compressedSizeInBytes );

    REQUIRE_EQUAL( index.compressedSizeInBytes, restoredIndex.compressedSizeInBytes );
    REQUIRE_EQUAL( index.uncompressedSizeInBytes, restoredIndex.uncompressedSizeInBytes );
    REQUIRE_EQUAL( index.checkpointSpacing, restoredIndex.checkpointSpacing );
    REQUIRE_EQUAL( index.windowSizeInBytes, restoredIndex.windowSizeInBytes );
    REQUIRE( index.checkpoints == restoredIndex.checkpoints );
    REQUIRE_EQUAL( static_cast<bool>( index.windows ), static_cast<bool>( restoredIndex.windows ) );

    if ( index.windows && restoredIndex.windows ) {
        REQUIRE_EQUAL( index.windows->size(), restoredIndex.windows->size() );
        if ( index.windows->size() == restoredIndex.windows->size() ) {
            const auto [lock, windowData] = index.windows->data();
            const auto [lock2, restoredWindowData] = restoredIndex.windows->data();

            size_t i{ 0 };
            for ( auto it = windowData->begin(), it2 = restoredWindowData->begin(); i < windowData->size();
                  ++it, ++it2, ++i )
            {
                REQUIRE( static_cast<bool>( it->second ) );
                if ( !it->second ) {
                    return;
                }

                const auto& [offset, window] = *it;
                const auto& [restoredOffset, restoredWindow] = *it2;

                if ( offset != restoredOffset ) {
                    std::cerr << "Window offsets at index: " << i << " differ: " << offset << " vs. restored: "
                              << restoredOffset << "\n";
                }

                if ( window->decompressedSize() != restoredWindow->decompressedSize() ) {
                    std::cerr << "Window decompressed sizes at index: " << i << " differ: "
                              << window->decompressedSize()<< " vs. restored: "
                              << restoredWindow->decompressedSize() << "\n";
                }

                if ( window->compressedData() != restoredWindow->compressedData() ) {
                    std::cerr << "Compressed window data at index: " << i << " differs!\n";
                    std::stringstream contents;

                    contents << "    Original: {" << std::hex;
                    for ( const auto& value : window->compressedData() ) {
                        contents << " " << static_cast<uint32_t>( value );
                    }
                    contents << " }\n";

                    contents << "    Restored: {" << std::hex;
                    for ( const auto& value : restoredWindow->compressedData() ) {
                        contents << " " << static_cast<uint32_t>( value );
                    }
                    contents << " }\n";

                    std::cerr << std::move( contents ).str();
                }
            }
        }

        /* Locks from WindowMap::data call above must be released before calling this! */
        REQUIRE( *index.windows == *restoredIndex.windows );
    }

    REQUIRE( index == restoredIndex );
}


int
main( int    argc,
      char** argv )
{
    if ( argc == 0 ) {
        std::cerr << "Expected at least the launch command as the first argument!\n";
        return 1;
    }

    testRandomAccessIndex();

    const std::string binaryFilePath( argv[0] );
    std::string binaryFolder = std::filesystem::path( binaryFilePath ).parent_path();
    if ( binaryFolder.empty() ) {
        binaryFolder = ".";
    }
    const auto rootFolder =
        static_cast<std::filesystem::path>(
            findParentFolderContaining( binaryFolder, "src/tests/data/base64-256KiB.bgz" )
        ) / "src" / "tests" / "data";

    testIndexReadWrite( rootFolder / "base64-256KiB.gz",
                        rootFolder / "base64-256KiB",
                        rootFolder / "base64-256KiB.gz.index" );
    testBzipIndexRead( rootFolder / "base64-256KiB.bgz",
                       rootFolder / "base64-256KiB",
                       rootFolder / "base64-256KiB.bgz.gzi" );

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors;
}
