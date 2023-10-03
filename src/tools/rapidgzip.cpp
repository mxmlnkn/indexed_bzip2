#include <cstdio>
#include <iostream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxopts.hpp>

#include <AffinityHelpers.hpp>
#include <common.hpp>
#include <filereader/Standard.hpp>
#include <FileUtils.hpp>
#include <GzipAnalyzer.hpp>
#include <rapidgzip.hpp>
#include <Statistics.hpp>

#include "CLIHelper.hpp"
#include "thirdparty.hpp"


class BrokenPipeException :
    public std::exception
{};


struct Arguments
{
    unsigned int decoderParallelism{ 0 };
    size_t chunkSize{ 4_Mi };
    std::string indexLoadPath;
    std::string indexSavePath;
    bool verbose{ false };
    bool crc32Enabled{ true };
};


void
printRapidgzipHelp( const cxxopts::Options& options )
{
    std::cout
    << options.help()
    << "\n"
    << "If no file names are given, rapidgzip decompresses from standard input to standard output.\n"
    << "If the output is discarded by piping to /dev/null, then the actual decoding step might\n"
    << "be omitted if neither -l nor -L nor --force are given.\n"
    << "\n"
    << "Examples:\n"
    << "\n"
    << "Decompress a file:\n"
    << "  rapidgzip -d file.gz\n"
    << "\n"
    << "Decompress a file in parallel:\n"
    << "  rapidgzip -d -P 0 file.gz\n"
    << "\n"
    << "List information about all gzip streams and deflate blocks:\n"
    << "  rapidgzip --analyze file.gz\n"
    << std::endl;
}


template<typename Reader>
void
printIndexAnalytics( const Reader& reader )
{
    const auto offsets = reader->blockOffsets();
    if ( offsets.size() <= 1 ) {
        return;
    }

    /* Analyze the offsets. */

    Statistics<double> encodedOffsetSpacings;
    Statistics<double> decodedOffsetSpacings;
    for ( auto it = offsets.begin(), nit = std::next( offsets.begin() ); nit != offsets.end(); ++it, ++nit ) {
        const auto& [encodedOffset, decodedOffset] = *it;
        const auto& [nextEncodedOffset, nextDecodedOffset] = *nit;
        if ( nextEncodedOffset - encodedOffset > 0 ) {
            encodedOffsetSpacings.merge( static_cast<double>( nextEncodedOffset - encodedOffset ) / CHAR_BIT / 1e6 );
            decodedOffsetSpacings.merge( static_cast<double>( nextDecodedOffset - decodedOffset ) / 1e6 );
        }
    }

    std::cerr
        << "[Seekpoints Index]\n"
        << "    Encoded offset spacings: ( min: " << encodedOffsetSpacings.min << ", "
        << encodedOffsetSpacings.formatAverageWithUncertainty()
        << ", max: " << encodedOffsetSpacings.max << " ) MB\n"
        << "    Decoded offset spacings: ( min: " << decodedOffsetSpacings.min << ", "
        << decodedOffsetSpacings.formatAverageWithUncertainty()
        << ", max: " << decodedOffsetSpacings.max << " ) MB\n";

    /* Analyze the windows. */
    const auto gzipIndex = reader->gzipIndex();
    if ( gzipIndex.windows ) {
        const auto [lock, windows] = gzipIndex.windows->data();
        const auto compressedWindowSize =
            std::accumulate( windows->begin(), windows->end(), size_t( 0 ), [] ( size_t sum, const auto& kv ) {
                return sum + ( kv.second ? kv.second->compressedSize() : 0 );
            } );
        const auto decompressedWindowSize =
            std::accumulate( windows->begin(), windows->end(), size_t( 0 ), [] ( size_t sum, const auto& kv ) {
                return sum + ( kv.second ? kv.second->decompressedSize() : 0 );
            } );
        std::cerr << "    Windows Count: " << windows->size() << "\n"
                  << "    Total Compressed Window Size: " << formatBytes( compressedWindowSize ) << "\n"
                  << "    Total Decompressed Window Size: " << formatBytes( decompressedWindowSize ) << "\n";
    }
}


enum class DecompressErrorCode
{
    SUCCESS = 0,
    BROKEN_PIPE = 1,
};


template<typename ChunkData,
         typename WriteFunctor = std::function<void ( const std::shared_ptr<ChunkData>&, size_t, size_t )> >
[[nodiscard]] std::pair<DecompressErrorCode, size_t>
decompressParallel( const Arguments&    args,
                    UniqueFileReader    inputFile,
                    const WriteFunctor& writeFunctor )
{
    using Reader = rapidgzip::ParallelGzipReader<ChunkData>;
    auto reader = std::make_unique<Reader>( std::move( inputFile ), args.decoderParallelism, args.chunkSize );

    reader->setStatisticsEnabled( args.verbose );
    reader->setShowProfileOnDestruction( args.verbose );
    reader->setCRC32Enabled( args.crc32Enabled );
    reader->setKeepIndex( !args.indexSavePath.empty() || !args.indexLoadPath.empty() );

    if ( !args.indexLoadPath.empty() ) {
        reader->importIndex( std::make_unique<StandardFileReader>( args.indexLoadPath ) );

        if ( args.verbose && ( !args.indexSavePath.empty() || !args.indexLoadPath.empty() ) ) {
            printIndexAnalytics( reader );
        }
    }

    size_t totalBytesRead{ 0 };
    try {
        totalBytesRead = reader->read( writeFunctor );
    } catch ( const BrokenPipeException& ) {
        return { DecompressErrorCode::BROKEN_PIPE, 0 };
    }

    if ( !args.indexSavePath.empty() ) {
        const auto file = throwingOpen( args.indexSavePath, "wb" );
        const auto checkedWrite =
            [&file] ( const void* buffer, size_t size )
            {
                if ( std::fwrite( buffer, 1, size, file.get() ) != size ) {
                    throw std::runtime_error( "Failed to write data to index!" );
                }
            };

        reader->exportIndex( checkedWrite );
    }

    if ( args.verbose && args.indexLoadPath.empty() && !args.indexSavePath.empty() ) {
        printIndexAnalytics( reader );
    }

    return { DecompressErrorCode::SUCCESS, totalBytesRead };
}


int
rapidgzipCLI( int                  argc,
              char const * const * argv )
{
    /* Cleaned, checked, and typed arguments. */
    Arguments args;

    /**
     * @note For some reason implicit values do not mix very well with positional parameters!
     *       Parameters given to arguments with implicit values will be matched by the positional argument instead!
     */
    cxxopts::Options options( "rapidgzip",
                              "A gzip decompressor tool based on the rapidgzip backend from ratarmount" );
    options.add_options( "Decompression Options" )
        ( "c,stdout"     , "Output to standard output. This is the default, when reading from standard input." )
        ( "f,force"      , "Force overwriting existing output files. "
                           "Also forces decompression even when piped to /dev/null." )
        ( "i,input"      , "Input file. If none is given, data is read from standard input.",
          cxxopts::value<std::string>() )
        ( "o,output"     ,
          "Output file. If none is given, use the input file name with '.gz' stripped or '<input file>.out'. "
          "If no input is read from standard input and not output file is given, then will write to standard output.",
          cxxopts::value<std::string>() )
        ( "k,keep"       , "Keep (do not delete) input file. Only for compatibility. "
                           "This tool will not delete anything automatically!" )
        ( "P,decoder-parallelism",
          "Use the parallel decoder. "
          "If an optional integer >= 1 is given, then that is the number of decoder threads to use. "
          "Note that there might be further threads being started with non-decoding work. "
          "If 0 is given, then the parallelism will be determined automatically.",
          cxxopts::value<unsigned int>()->default_value( "0" ) );

    options.add_options( "Advanced" )
        ( "chunk-size", "The chunk size decoded by the parallel workers in KiB.",
          cxxopts::value<unsigned int>()->default_value( "4096" ) )
        ( "verify", "Verify CRC32 checksum. Will slow down decompression and there are already some implicit "
                    "and explicit checks like whether the end of the file could be reached and whether the stream "
                    "size is correct. ",
          cxxopts::value( args.crc32Enabled )->implicit_value( "true" ) )
        ( "no-verify", "Do not verify CRC32 checksum. Might speed up decompression and there are already some implicit "
                       "and explicit checks like whether the end of the file could be reached and whether the stream "
                       "size is correct.",
          cxxopts::value( args.crc32Enabled )->implicit_value( "false" ) )
        ( "io-read-method", "Option to force a certain I/O method for reading. By default, pread will be used "
                            "when possible. Possible values: pread, sequential, locked-read",
          cxxopts::value<std::string>()->default_value( "pread" ) );

    options.add_options( "Output Options" )
        ( "h,help"   , "Print this help message." )
        ( "q,quiet"  , "Suppress noncritical error messages." )
        ( "v,verbose", "Print debug output and profiling statistics." )
        ( "V,version", "Display software version." )
        ( "oss-attributions", "Display open-source software licenses." )
        ( "oss-attributions-yaml", "Display open-source software licenses in YAML format for use with Conda." );

    /* These options are offered because just piping to other tools can already bottleneck everything! */
    options.add_options( "Actions" )
        ( "d,decompress" , "Force decompression. Only for compatibility. No compression supported anyways." )
        ( "import-index" , "Uses an existing gzip index.", cxxopts::value<std::string>() )
        ( "export-index" , "Write out a gzip index file.", cxxopts::value<std::string>() )
        ( "count"        , "Prints the decompressed size." )
        ( "l,count-lines", "Prints the number of newline characters in the decompressed data." )
        ( "analyze"      , "Print output about the internal file format structure like the block types." );

    options.parse_positional( { "input" } );

    /* cxxopts allows to specify arguments multiple times. But if the argument type is not a vector, then only
     * the last value will be kept! Therefore, do not check against this usage and simply use that value.
     * Arguments may only be queried with as if they have (default) values. */

    const auto parsedArgs = options.parse( argc, argv );

    const auto force = parsedArgs["force"].as<bool>();
    const auto quiet = parsedArgs["quiet"].as<bool>();
    args.verbose = parsedArgs["verbose"].as<bool>();

    const auto getParallelism = [] ( const auto p ) { return p > 0 ? p : availableCores(); };
    args.decoderParallelism = getParallelism( parsedArgs["decoder-parallelism"].as<unsigned int>() );

    if ( args.verbose ) {
        for ( auto const* const path : { "input", "output" } ) {
            std::string value = "<none>";
            try {
                value = parsedArgs[path].as<std::string>();
            } catch ( ... ) {}
            std::cerr << "file path for " << path << ": " << value << "\n";
        }
    }

    /* Check against simple commands like help and version. */

    if ( parsedArgs.count( "help" ) > 0 ) {
        printRapidgzipHelp( options );
        return 0;
    }

    if ( parsedArgs.count( "version" ) > 0 ) {
        std::cout
            << "rapidgzip, CLI to the parallelized, indexed, and seekable gzip decoding library rapidgzip version "
            << static_cast<uint32_t>( rapidgzip::VERSION[0] ) << "."
            << static_cast<uint32_t>( rapidgzip::VERSION[1] ) << "."
            << static_cast<uint32_t>( rapidgzip::VERSION[2] ) << "\n";
        return 0;
    }

    if ( parsedArgs.count( "oss-attributions" ) > 0 ) {
        std::cout
            << "# " << thirdparty::cxxopts::name << "\n\n"
            << thirdparty::cxxopts::url << "\n\n"
            << thirdparty::cxxopts::license << "\n\n"
        #ifdef WITH_ISAL
            << "# " << thirdparty::isal::name << "\n\n"
            << thirdparty::isal::url << "\n\n"
            << thirdparty::isal::license << "\n\n"
        #endif
        #ifdef WITH_RPMALLOC
            << "# " << thirdparty::rpmalloc::name << "\n\n"
            << thirdparty::rpmalloc::url << "\n\n"
            << thirdparty::rpmalloc::fullLicense << "\n\n"
        #endif
            << "# " << thirdparty::zlib::name << "\n\n"
            << thirdparty::zlib::url << "\n\n"
            << thirdparty::zlib::license;
        return 0;
    }

    if ( parsedArgs.count( "oss-attributions-yaml" ) > 0 ) {
        std::cout
            << "root_name: rapidgzip\n"
            << "third_party_libraries:\n"
            << "  - package_name: " << thirdparty::cxxopts::name << "\n"
            << "    package_version: "
            << static_cast<uint32_t>( cxxopts::version.major ) << "."
            << static_cast<uint32_t>( cxxopts::version.minor ) << "."
            << static_cast<uint32_t>( cxxopts::version.patch ) << "\n"
            << "    license: Unlicense\n"
            << "    licenses:\n"
            << "      - license: Unlicense\n"
            << "        text: " << toYamlString( thirdparty::cxxopts::license ) << "\n"
        #ifdef WITH_ISAL
            << "  - package_name: " << thirdparty::isal::name << "\n"
            << "    package_version: 2.30.0\n"
            << "    license: BSD-3\n"
            << "    licenses:\n"
            << "      - license: BSD-3\n"
            << "        text: " << toYamlString( thirdparty::isal::license ) << "\n"
        #endif
        #ifdef WITH_RPMALLOC
            << "  - package_name: " << thirdparty::rpmalloc::name << "\n"
            << "    package_version: 1.4.4\n"
            << "    license: Unlicense/MIT\n"
            << "    licenses:\n"
            << "      - license: Unlicense\n"
            << "        text: " << toYamlString( thirdparty::rpmalloc::unlicense ) << "\n"
            << "      - license: MIT\n"
            << "        text: " << toYamlString( thirdparty::rpmalloc::mit ) << "\n"
        #endif
            << "  - package_name: " << thirdparty::zlib::name << "\n"
            << "    package_version: " << ZLIB_VER_MAJOR << "." << ZLIB_VER_MINOR << "." << ZLIB_VER_REVISION << "\n"
            << "    license: Zlib\n"
            << "    licenses:\n"
            << "      - license: Zlib\n"
            << "        text: " << toYamlString( thirdparty::zlib::license ) << "\n";
        return 0;
    }

    /* Parse input file specifications. */

    if ( parsedArgs.count( "input" ) > 1 ) {
        std::cerr << "One or none gzip filename to decompress must be specified!\n";
        return 1;
    }

    if ( !stdinHasInput() && ( parsedArgs.count( "input" ) != 1 ) ) {
        std::cerr << "Either stdin must have input, e.g., by piping to it, or an input file must be specified!\n";
        return 1;
    }

    std::string inputFilePath;  /* Can be empty. Then, read from STDIN. */
    if ( parsedArgs.count( "input" ) == 1 ) {
        inputFilePath = parsedArgs["input"].as<std::string>();
        if ( !inputFilePath.empty() && !fileExists( inputFilePath ) ) {
            std::cerr << "Input file could not be found! Specified path: " << inputFilePath << "\n";
            return 1;
        }
    }

    auto inputFile = openFileOrStdin( inputFilePath );
    const auto ioReadMethod = parsedArgs["io-read-method"].as<std::string>();
    if ( ioReadMethod == "sequential" ) {
        inputFile = std::make_unique<SinglePassFileReader>( std::move( inputFile ) );
    } else if ( ( ioReadMethod == "locked-read" ) || ( ioReadMethod == "pread" ) ) {
        auto sharedFile = ensureSharedFileReader( std::move( inputFile ) );
        sharedFile->setUsePread( ioReadMethod == "pread" );
        inputFile = std::move( sharedFile );
    }

    /* Check if analysis is requested. */

    if ( parsedArgs.count( "analyze" ) > 0 ) {
        return rapidgzip::deflate::analyze( std::move( inputFile ), args.verbose ) == rapidgzip::Error::NONE ? 0 : 1;
    }

    /* Parse action arguments. */

    const auto countBytes = parsedArgs.count( "count" ) > 0;
    const auto countLines = parsedArgs.count( "count-lines" ) > 0;
    const auto decompress = parsedArgs.count( "decompress" ) > 0;

    /* Parse output file specifications. */

    auto outputFilePath = getFilePath( parsedArgs, "output" );
    /* Automatically determine output file path if none has been given and not writing to stdout. */
    if ( ( parsedArgs.count( "stdout" ) == 0 ) && outputFilePath.empty() && !inputFilePath.empty() ) {
        const std::string& suffix = ".gz";
        if ( endsWith( inputFilePath, suffix, /* case sensitive */ false ) ) {
            outputFilePath = std::string( inputFilePath.begin(),
                                          inputFilePath.end()
                                          - static_cast<std::string::difference_type>( suffix.size() ) );
        } else {
            outputFilePath = inputFilePath + ".out";
            if ( !quiet && decompress ) {
                std::cerr << "[Warning] Could not deduce output file name. Will write to '" << outputFilePath << "'\n";
            }
        }
    }

    /* Parse other arguments. */

    if ( decompress && ( outputFilePath != "/dev/null" ) && fileExists( outputFilePath ) && !force ) {
        std::cerr << "Output file '" << outputFilePath << "' already exists! Use --force to overwrite.\n";
        return 1;
    }

    args.indexLoadPath = parsedArgs.count( "import-index" ) > 0
                         ? parsedArgs["import-index"].as<std::string>()
                         : std::string();
    args.indexSavePath = parsedArgs.count( "export-index" ) > 0
                         ? parsedArgs["export-index"].as<std::string>()
                         : std::string();
    if ( !args.indexLoadPath.empty() && !args.indexSavePath.empty() ) {
        std::cerr << "[Warning] Importing and exporting an index makes limited sense.\n";
    }
    if ( ( !args.indexLoadPath.empty() || !args.indexSavePath.empty() ) && ( args.decoderParallelism == 1 ) ) {
        std::cerr << "[Warning] The index only has an effect for parallel decoding.\n";
    }
    if ( !args.indexLoadPath.empty() && !fileExists( args.indexLoadPath ) ) {
        std::cerr << "The index to import was not found!\n";
        return 1;
    }

    /*
    Read 340425 checkpoints
        Windows Count: 340425
        Total Window Size: 1 GiB 652 MiB 217 KiB 518 B


    Read 340425 checkpoints
    Failed to get sparse window for 872343404952 with error: Failed to decode the deflate block header! End of file reached.. Will ignore it.
        Windows Count: 340424
        Total Window Size: 747 MiB 794 KiB 512 B

    real	6m22.796s
    user	1m6.975s
    sys	0m33.980s


        -> Not as large a reduction as hoped for but 2x still is nice

    Read 340425 checkpoints
    Failed to get sparse window for 872343404952 with error: Failed to decode the deflate block header! End of file reached.. Will ignore it.
        Windows Count: 340424
        Total Window Size: 609 MiB 568 KiB 423 B

    real	6m29.877s
    user	1m14.430s
    sys	0m33.450s

     -> level 1 (without buffer) instead of level 0! It really is worth it at this point

    Read 340425 checkpoints
    Failed to get sparse window for 872343404952 with error: Failed to decode the deflate block header! End of file reached.. Will ignore it.
        Windows Count: 340424
        Total Window Size: 608 MiB 126 KiB 654 B

     -> level 2 instead of level 0. Not worth it

    ISA-L (level 1)
        Windows Count: 340424
        Total Window Size: 609 MiB 568 KiB 422 B
        Total Window Size Batch-Compressed: 609 MiB 566 KiB 856 B
        Total Window Size Without Zeros: 608 MiB 212 KiB 519 B

    zlib (default level (6))

        Windows Count: 340424
        Total Window Size: 528 MiB 323 KiB 15 B
        Total Window Size Batch-Compressed: 528 MiB 320 KiB 519 B
        Total Window Size Without Zeros: 527 MiB 507 KiB 103 B

    real	11m30.027s
    user	7m36.295s
    sys	0m34.736s



    m rapidgzip && time src/tools/rapidgzip --import-index 4GiB-base64.gz{.index,}
    ninja: no work to do.
    Read 780 checkpoints
    Failed to get sparse window for 26111249624 with error: Failed to decode the deflate block header! End of file reached.. Will ignore it.
        Windows Count: 779
        Total Window Size: 632 KiB 301 B

    real	0m0.357s
    user	0m0.221s
    sys	0m0.032s


    stat 4GiB-base64.gz.index
      File: 4GiB-base64.gz.index
      Size: 25540347  	Blocks: 49888      IO Block: 4096   regular file

    ISA-L
        Windows Count: 779
        Total Window Size: 439 KiB 44 B
        Total Window Size Batch-Compressed: 438 KiB 216 B
        Total Window Size Without Zeros: 417 KiB 37 B

    Zlib:
        Windows Count: 779
        Total Window Size: 338 KiB 553 B
        Total Window Size Batch-Compressed: 337 KiB 479 B
        Total Window Size Without Zeros: 336 KiB 538 B
    */
    if ( !args.indexLoadPath.empty() ) {
        const auto file = ensureSharedFileReader( std::move( inputFile ) );
        auto indexFile = std::make_unique<StandardFileReader>( args.indexLoadPath );
        const auto index = readGzipIndex( std::move( indexFile ), file->clone() );

        std::cerr << "Read " << index.checkpoints.size() << " checkpoints\n";

        if ( !index.windows ) {
            throw std::logic_error( "There should be a valid window map!" );
        }

        size_t windowSize2{ 0 };
        size_t windowSize3{ 0 };
        size_t windowSize4{ 0 };
        /* compress windows in batches. */
        std::vector<uint8_t> allWindows;
        std::vector<uint8_t> allWindows4;
        size_t windowBatchCount{ 0 };

        std::array<uint8_t, 64_Ki> windowPatches;

        rapidgzip::BitReader bitReader( file->clone() );
        WindowMap windows;
        for ( const auto& checkpoint : index.checkpoints ) {
            const auto fullWindow = index.windows->get( checkpoint.compressedOffsetInBits );
            if ( !fullWindow ) {
                throw std::logic_error( "Windows to all checkpoints should exist!" );
            }

            if ( fullWindow->empty() ) {
                windows.emplace( checkpoint.compressedOffsetInBits, *fullWindow );
                continue;
            }


            try {
                bitReader.seek( checkpoint.compressedOffsetInBits );
                const auto sparseWindow = rapidgzip::deflate::getSparseWindow( bitReader, *fullWindow );
                windows.emplace( checkpoint.compressedOffsetInBits, sparseWindow );

                allWindows.insert( allWindows.end(), sparseWindow.begin(), sparseWindow.end() );
                if ( ++windowBatchCount >= 16 ) {
                    windowSize2 += rapidgzip::compressWithIsal( allWindows ).size();
                    allWindows.clear();
                }

                /** @todo this only works for the .json file, else we need to adjust getSparseWindow.
                 * Format: <length zeros (may be 0)> <length data> <data> <length zeros> ...
                 */
                size_t targetSize{ 0 };
                bool lookingForZeros{ true };
                size_t length{ 0 };
                for ( size_t i = 0; i < sparseWindow.size(); ++i ) {
                    const auto isZero = sparseWindow[i] == 0;

                    if ( isZero != lookingForZeros ) {
                        windowPatches[targetSize++] = static_cast<uint8_t>( length );
                        lookingForZeros = false;
                        length = 0;
                    }

                    if ( !isZero ) {
                        windowPatches[targetSize++] = sparseWindow[i];
                    }
                }

                windowSize3 += rapidgzip::compressWithIsal( { windowPatches.data(), targetSize } ).size();

                allWindows4.insert( allWindows4.end(), windowPatches.begin(), windowPatches.begin() + targetSize );
                if ( ++windowBatchCount >= 16 ) {
                    windowSize4 += rapidgzip::compressWithIsal( allWindows4 ).size();
                    allWindows4.clear();
                }
            } catch ( const std::exception& exception ) {
                std::cerr << "Failed to get sparse window for " << checkpoint.compressedOffsetInBits << " with error: "
                          << exception.what() << ". Will ignore it.\n";
            }
        }

        /* Analyze the windows. */
        const auto [lock, windowMap] = windows.data();
        const auto windowSize =
            std::accumulate( windowMap->begin(), windowMap->end(), size_t( 0 ),
                             [] ( size_t sum, const auto& kv ) { return sum + kv.second.compressedSize(); } );
        std::cerr << "    Windows Count: " << windowMap->size() << "\n"
                  << "    Total Window Size: " << formatBytes( windowSize ) << "\n"
                  << "    Total Window Size Batch-Compressed: " << formatBytes( windowSize2 ) << "\n"
                  << "    Total Window Size Without Zeros: " << formatBytes( windowSize3 ) << "\n"
                  << "    Total Window Size Without Zeros Batch-Compressed: " << formatBytes( windowSize4 ) << "\n";

        return 0;
    }

    /* Actually do things as requested. */

    if ( decompress || countBytes || countLines || !args.indexSavePath.empty() ) {
        if ( decompress && args.verbose ) {
            std::cerr << "Decompress " << ( inputFilePath.empty() ? "<stdin>" : inputFilePath.c_str() )
                      << " -> " << ( outputFilePath.empty() ? "<stdout>" : outputFilePath.c_str() ) << "\n";
        }

        if ( !inputFile ) {
            std::cerr << "Could not open input file: " << inputFilePath << "!\n";
            return 1;
        }

        std::unique_ptr<OutputFile> outputFile;
        if ( decompress ) {
            outputFile = std::make_unique<OutputFile>( outputFilePath );
        }
        const auto outputFileDescriptor = outputFile ? outputFile->fd() : -1;

        uint64_t newlineCount{ 0 };

        const auto t0 = now();

        const auto writeAndCount =
            [outputFileDescriptor, countLines, &newlineCount]
            ( const std::shared_ptr<rapidgzip::ChunkData>& chunkData,
              size_t const                               offsetInBlock,
              size_t const                               dataToWriteSize )
            {
                const auto errorCode = writeAll( chunkData, outputFileDescriptor, offsetInBlock, dataToWriteSize );
                if ( errorCode == EPIPE ) {
                    throw BrokenPipeException();
                }
                if ( errorCode != 0 ) {
                    std::stringstream message;
                    message << "Failed to write all bytes because of: " << strerror( errorCode )
                            << " (" << errorCode << ")";
                    throw std::runtime_error( std::move( message.str() ) );
                }

                if ( countLines ) {
                    using rapidgzip::deflate::DecodedData;
                    for ( auto it = DecodedData::Iterator( *chunkData, offsetInBlock, dataToWriteSize );
                          static_cast<bool>( it ); ++it )
                    {
                        const auto& [buffer, size] = *it;
                        newlineCount += countNewlines( { reinterpret_cast<const char*>( buffer ), size } );
                    }
                }
            };

        args.chunkSize = parsedArgs["chunk-size"].as<unsigned int>() * 1_Ki;

        auto errorCode = DecompressErrorCode::SUCCESS;
        size_t totalBytesRead{ 0 };
        if ( ( outputFileDescriptor == -1 ) && args.indexSavePath.empty() && countBytes && !countLines
             && !args.crc32Enabled )
        {
            /* Need to do nothing with the chunks because decompressParallel returns the decompressed size.
             * Note that we use rapidgzip::ChunkDataCounter to speed up decompression. Therefore an index
             * will not be created and there also will be no checksum verification! */
            std::tie( errorCode, totalBytesRead ) = decompressParallel<rapidgzip::ChunkDataCounter>(
                args, std::move( inputFile ), /* do nothing */ {} );
        } else {
            std::function<void( const std::shared_ptr<rapidgzip::ChunkData>, size_t, size_t )> writeFunctor;
            /* Else, do nothing. An empty functor will lead to decompression to be skipped if the index is finalized! */
            if ( ( outputFileDescriptor != -1 ) || countLines ) {
                writeFunctor = writeAndCount;
            }
            std::tie( errorCode, totalBytesRead ) = decompressParallel<rapidgzip::ChunkData>(
                args, std::move( inputFile ), writeFunctor );
        }

        const auto writeToStdErr = outputFile && outputFile->writingToStdout();
        if ( outputFile ) {
            outputFile->truncate( totalBytesRead );
            outputFile.reset();  // Close the file here to include it in the time measurement.
        }

        const auto t1 = now();
        if ( args.verbose ) {
            std::cerr << "Decompressed in total " << totalBytesRead << " B in " << duration( t0, t1 ) << " s -> "
                      << static_cast<double>( totalBytesRead ) / 1e6 / duration( t0, t1 ) << " MB/s\n";
        }

        auto& out = writeToStdErr ? std::cerr : std::cout;
        if ( countBytes != countLines ) {
            out << ( countBytes ? totalBytesRead : newlineCount );
        } else if ( countBytes && countLines ) {
            out << "Size: " << totalBytesRead << "\n";
            out << "Lines: " << newlineCount << "\n";
        }

        switch ( errorCode )
        {
        case DecompressErrorCode::BROKEN_PIPE:
            return 128 + /* SIGPIPE */ 13;
        case DecompressErrorCode::SUCCESS:
            return 0;
        }
        return 2;
    }

    std::cerr << "No suitable arguments were given. Please refer to the help!\n\n";

    printRapidgzipHelp( options );

    return 1;
}


#if !defined( WITH_PYTHON_SUPPORT ) && !defined( WITHOUT_MAIN )
int
main( int argc, char** argv )
{
    try
    {
        return rapidgzipCLI( argc, argv );
    }
    catch ( const rapidgzip::BitReader::EndOfFileReached& exception )
    {
        std::cerr << "Unexpected end of file. Truncated or invalid gzip?\n";
        return 1;
    }
    catch ( const std::exception& exception )
    {
        const std::string_view message{ exception.what() };
        if ( message.empty() ) {
            std::cerr << "Caught exception with typeid: " << typeid( exception ).name() << "\n";
        } else {
            std::cerr << "Caught exception: " << message << "\n";
        }
        return 1;
    }

    return 1;
}
#endif
