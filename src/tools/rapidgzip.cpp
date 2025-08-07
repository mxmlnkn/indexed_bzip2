#include <climits>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxopts.hpp>

#include <core/AffinityHelpers.hpp>
#include <core/common.hpp>
#include <core/FileRanges.hpp>
#include <core/FileUtils.hpp>
#include <core/Statistics.hpp>
#include <filereader/Standard.hpp>
#include <rapidgzip/gzip/GzipAnalyzer.hpp>
#include <rapidgzip/rapidgzip.hpp>

#include "CLIHelper.hpp"
#include "thirdparty.hpp"


using namespace rapidgzip;


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
    bool keepIndex{ false };
    bool windowSparsity{ true };
    bool gatherLineOffsets{ false };
    IndexFormat indexFormat{ IndexFormat::INDEXED_GZIP };
    std::optional<std::vector<FileRange> > fileRanges{};
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
         typename ReadFunctor = std::function<size_t( std::unique_ptr<rapidgzip::ParallelGzipReader<ChunkData> >& )> >
[[nodiscard]] DecompressErrorCode
decompressParallel( const Arguments&   args,
                    UniqueFileReader   inputFile,
                    const ReadFunctor& readFunctor )
{
    using Reader = rapidgzip::ParallelGzipReader<ChunkData>;
    auto reader = std::make_unique<Reader>( std::move( inputFile ), args.decoderParallelism, args.chunkSize );

    reader->setStatisticsEnabled( args.verbose );
    reader->setShowProfileOnDestruction( args.verbose );
    reader->setCRC32Enabled( args.crc32Enabled );
    reader->setKeepIndex( !args.indexSavePath.empty() || !args.indexLoadPath.empty() || args.keepIndex );
    reader->setWindowSparsity( args.windowSparsity );
    if ( ( args.indexFormat == IndexFormat::GZTOOL ) || ( args.indexFormat == IndexFormat::GZTOOL_WITH_LINES ) ) {
        /* Compress with zlib instead of gzip to avoid recompressions when exporting the index. */
        reader->setWindowCompressionType( CompressionType::ZLIB );
    }

    if ( !args.indexLoadPath.empty() ) {
        reader->importIndex( std::make_unique<StandardFileReader>( args.indexLoadPath ) );

        if ( args.verbose && ( !args.indexSavePath.empty() || !args.indexLoadPath.empty() ) ) {
            printIndexAnalytics( reader );
        }
    }

    if ( args.gatherLineOffsets
         || ( !args.indexSavePath.empty() && ( args.indexFormat == IndexFormat::GZTOOL_WITH_LINES ) ) )
    {
        reader->gatherLineOffsets();
    }

    try {
        readFunctor( reader );
    } catch ( const BrokenPipeException& ) {
        return DecompressErrorCode::BROKEN_PIPE;
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

        reader->exportIndex( checkedWrite, args.indexFormat );
    }

    if ( args.verbose && args.indexLoadPath.empty() && !args.indexSavePath.empty() ) {
        printIndexAnalytics( reader );
    }

    return DecompressErrorCode::SUCCESS;
}


[[nodiscard]] std::pair<std::string, UniqueFileReader>
parseInputFileSpecification( const cxxopts::ParseResult& parsedArgs )
{
    if ( parsedArgs.count( "input" ) > 1 ) {
        std::cerr << "One or none gzip filename to decompress must be specified!\n";
        return {};
    }

    std::string inputFilePath;  /* Can be empty. Then, read from STDIN. */
    if ( parsedArgs.count( "input" ) == 1 ) {
        inputFilePath = parsedArgs["input"].as<std::string>();
        if ( !inputFilePath.empty() && !fileExists( inputFilePath ) ) {
            std::cerr << "Input file could not be found! Specified path: " << inputFilePath << "\n";
            return {};
        }
    }

    if ( inputFilePath.empty() && !stdinHasInput() ) {
        std::cerr << "Either stdin must have input, e.g., by piping to it, or an input file must be specified!\n";
        return {};
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

    return { inputFilePath, std::move( inputFile ) };
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
    options.add_options( "Decompression" )
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
          cxxopts::value<unsigned int>()->default_value( "0" ) )
        ( "ranges", "Decompress only the specified byte ranges. Example: 10@0,1KiB@15KiB,5L@20L,inf@40L to decompress "
                    "the first 10 bytes, 1024 bytes at offset 15 KiB, as well as the 5 lines after skipping the first "
                    "20 lines, and lastly, everything after skipping the first 40.",
          cxxopts::value<std::string>() );

    options.add_options( "Advanced" )
        ( "chunk-size", "The chunk size decoded by the parallel workers in KiB.",
          cxxopts::value<unsigned int>()->default_value( "4096" ) )
        ( "verify", "Verify CRC32 checksum during decompression. Will slow down decompression and there are already "
                    "some implicit and explicit checks like whether the end of the file could be reached and whether "
                    "the stream size is correct. If no action is specified, this will do nothing. Use --test to "
                    "force checksum verification.",
          cxxopts::value( args.crc32Enabled )->implicit_value( "true" ) )
        ( "no-verify", "Do not verify CRC32 checksum during decompression. Might speed up decompression and there are "
                       "already some implicit and explicit checks like whether the end of the file could be reached "
                       "and whether the stream size is correct.",
          cxxopts::value( args.crc32Enabled )->implicit_value( "false" ) )
        ( "io-read-method", "Option to force a certain I/O method for reading. By default, pread will be used "
                            "when possible. Possible values: pread, sequential, locked-read",
          cxxopts::value<std::string>()->default_value( "pread" ) )
        ( "index-format", "Option to select an output index format. Possible values: gztool, gztool-with-lines, "
                          "indexed_gzip.",
          cxxopts::value<std::string>()->default_value( "indexed_gzip" ) )
        ( "sparse-windows", "On by default. Reduce index compressibility by zeroing out window data that is not "
                            "referenced in the subsequent stream.",
          cxxopts::value( args.windowSparsity )->implicit_value( "true" ) )
        ( "no-sparse-windows", "Do not zero out unreferenced data in index windows. This may very slightly improve "
                               "first-time decompression speed, but may also lead to higher memory usage. It can also "
                               "be useful to create exported index that are binary identical to other those created "
                               "by other tools such as gztool to debug indexes. In normal usage the sparsity is an "
                               "index modification that is fully compatible with the original tools.",
          cxxopts::value( args.windowSparsity )->implicit_value( "false" ) );

    options.add_options( "Output" )
        ( "h,help"   , "Print this help message." )
        ( "q,quiet"  , "Suppress non-critical error messages." )
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
        ( "analyze"      , "Print output about the internal file format structure like the block types." )
        ( "t,test"       , "Verify decompression and checksums, if not disabled with --no-verify. "
                           "Note that this does not have any additional effect when combined with --decompress "
                           "or --ranges because it is equivalent to --decompress -o /dev/null." );

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
        #ifdef LIBRAPIDARCHIVE_WITH_ISAL
            << "# " << thirdparty::isal::name << "\n\n"
            << thirdparty::isal::url << "\n\n"
            << thirdparty::isal::license << "\n\n"
        #endif
        #ifdef LIBRAPIDARCHIVE_WITH_RPMALLOC
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
        #ifdef LIBRAPIDARCHIVE_WITH_ISAL
            << "  - package_name: " << thirdparty::isal::name << "\n"
            << "    package_version: 2.30.0\n"
            << "    license: BSD-3\n"
            << "    licenses:\n"
            << "      - license: BSD-3\n"
            << "        text: " << toYamlString( thirdparty::isal::license ) << "\n"
        #endif
        #ifdef LIBRAPIDARCHIVE_WITH_RPMALLOC
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

    /* Parse action arguments. */

    const auto countBytes = parsedArgs.count( "count" ) > 0;
    const auto countLines = parsedArgs.count( "count-lines" ) > 0;
    const auto doTest = parsedArgs.count( "test" ) > 0;
    const auto writeToStdOut = parsedArgs.count( "stdout" ) > 0;
    const auto decompress = ( parsedArgs.count( "decompress" ) > 0 ) || ( parsedArgs.count( "ranges" ) > 0 ) || doTest;

    /* Parse ranges. */
    if ( parsedArgs.count( "ranges" ) > 0 ) {
        args.fileRanges = parseFileRanges( parsedArgs["ranges"].as<std::string>() );

        /* Check whether the index needs to be kept because the ranges do not traverse the file in order. */
        if ( args.fileRanges->size() > 1 ) {
            for ( size_t i = 0; i + 1 < args.fileRanges->size(); ++i ) {
                if ( args.fileRanges->at( i ).offset + args.fileRanges->at( i ).size > args.fileRanges->at( i + 1 ).offset ) {
                    args.keepIndex = true;
                    break;
                }
            }
        }

        /* Check whether some ranges are lines and enable line offset gathering in that case. */
        for ( const auto& range : *args.fileRanges ) {
            if ( ( range.size > 0 ) && ( range.offsetIsLine || range.sizeIsLine ) ) {
                /* Because we cannot arbitrarily convert lines to offsets, we cannot easily determine whether
                 * backward seeking is necessary. Therefore keep the index if line offsets are used. */
                args.keepIndex = true;
                args.gatherLineOffsets = true;
                break;
            }
        }
    }

    /* Parse input file specifications. */

    auto [inputFilePath, inputFile] = parseInputFileSpecification( parsedArgs );
    if ( !inputFile ) {
        return 1;
    }

    /* Parse output file specifications. */

    auto outputFilePath = getFilePath( parsedArgs, "output" );
    /* Automatically determine output file path if none has been given and not writing to stdout. */
    if ( outputFilePath.empty() && !inputFilePath.empty() && !doTest && !writeToStdOut ) {
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

    if ( decompress && ( outputFilePath != "/dev/null" ) && fileExists( outputFilePath ) && !force ) {
        std::cerr << "Output file '" << outputFilePath << "' already exists! Use --force to overwrite.\n";
        return 1;
    }

    /* Parse index arguments. */

    args.indexLoadPath = parsedArgs.count( "import-index" ) > 0
                         ? parsedArgs["import-index"].as<std::string>()
                         : std::string();
    args.indexSavePath = parsedArgs.count( "export-index" ) > 0
                         ? parsedArgs["export-index"].as<std::string>()
                         : std::string();
    if ( !args.indexLoadPath.empty() && args.indexSavePath.empty() && ( args.decoderParallelism == 1 ) && !quiet ) {
        std::cerr << "[Warning] The index only has an effect for parallel decoding and index exporting.\n";
    }
    if ( !args.indexLoadPath.empty() && !fileExists( args.indexLoadPath ) ) {
        std::cerr << "The index to import was not found!\n";
        return 1;
    }

    if ( parsedArgs.count( "index-format" ) > 0 ) {
        const auto indexFormat = parsedArgs["index-format"].as<std::string>();
        if ( indexFormat == "indexed_gzip" ) {
            args.indexFormat = IndexFormat::INDEXED_GZIP;
        } else if ( indexFormat == "gztool" ) {
            args.indexFormat = IndexFormat::GZTOOL;
        } else if ( indexFormat == "gztool-with-lines" ) {
            args.indexFormat = IndexFormat::GZTOOL_WITH_LINES;
        } else {
            throw std::invalid_argument( "Invalid index format string: " + indexFormat );
        }

        if ( args.indexSavePath.empty() && !quiet ) {
            std::cerr << "[Warning] The index format has no effect without --export-index!\n";
        }
    }

    /* Check if analysis is requested. */

    if ( parsedArgs.count( "analyze" ) > 0 ) {
        return rapidgzip::deflate::analyze( std::move( inputFile ), args.verbose ) == rapidgzip::Error::NONE ? 0 : 1;
    }

    /* Check that there is at least one actionable request. */

    if ( !decompress && !countBytes && !countLines && args.indexSavePath.empty() ) {
        std::cerr << "No suitable arguments were given. Please refer to the help!\n\n";
        printRapidgzipHelp( options );
        return 1;
    }

    /* Actually do things as requested. */

    if ( decompress && !doTest && args.verbose ) {
        std::cerr << "Decompress " << ( inputFilePath.empty() ? "<stdin>" : inputFilePath.c_str() )
                  << " -> " << ( writeToStdOut ? "<stdout>" : outputFilePath.c_str() ) << "\n";
    }

    if ( !inputFile ) {
        std::cerr << "Could not open input file: " << inputFilePath << "!\n";
        return 1;
    }

    std::unique_ptr<OutputFile> outputFile;
    std::unique_ptr<OutputFile> stdoutFile;
    if ( decompress ) {
        if ( writeToStdOut ) {
            stdoutFile = std::make_unique<OutputFile>( /* empty path implies stdout */ "" );
        }
        if ( !outputFilePath.empty() ) {
            outputFile = std::make_unique<OutputFile>( outputFilePath );
        }
    }
    const auto outputFileDescriptor = outputFile ? outputFile->fd() : -1;
    const auto stdoutFileDescriptor = stdoutFile ? stdoutFile->fd() : -1;

    uint64_t newlineCount{ 0 };

    const auto t0 = now();

    size_t totalBytesRead{ 0 };
    const auto writeAndCount =
        [outputFileDescriptor, stdoutFileDescriptor, countLines, &newlineCount, &totalBytesRead]
        ( const std::shared_ptr<rapidgzip::ChunkData>& chunkData,
          size_t const                                 offsetInChunk,
          size_t const                                 dataToWriteSize )
        {
            for ( const auto fileDescriptor : { stdoutFileDescriptor, outputFileDescriptor } ) {
                if ( fileDescriptor == -1 ) {
                    continue;
                }

                const auto errorCode = writeAll( chunkData, fileDescriptor, offsetInChunk, dataToWriteSize );
                if ( errorCode == EPIPE ) {
                    throw BrokenPipeException();
                }

                if ( errorCode != 0 ) {
                    std::stringstream message;
                    message << "Failed to write all bytes because of: " << strerror( errorCode )
                            << " (" << errorCode << ")";
                    throw std::runtime_error( std::move( message ).str() );
                }
            }

            totalBytesRead += dataToWriteSize;

            if ( countLines ) {
                using rapidgzip::deflate::DecodedData;
                for ( auto it = DecodedData::Iterator( *chunkData, offsetInChunk, dataToWriteSize );
                      static_cast<bool>( it ); ++it )
                {
                    const auto& [buffer, size] = *it;
                    newlineCount += countNewlines( { reinterpret_cast<const char*>( buffer ), size } );
                }
            }
        };

    args.chunkSize = parsedArgs["chunk-size"].as<unsigned int>() * 1_Ki;

    auto errorCode = DecompressErrorCode::SUCCESS;
    const auto hasOutputFiles = ( outputFileDescriptor != -1 ) || ( stdoutFileDescriptor != -1 );
    if ( args.indexSavePath.empty() && countBytes && !countLines && !decompress && !hasOutputFiles ) {
        /* Need to do nothing with the chunks because decompressParallel returns the decompressed size.
         * Note that we use rapidgzip::ChunkDataCounter to speed up decompression. Therefore an index
         * will not be created and there also will be no checksum verification! */
        args.crc32Enabled = false;
        errorCode = decompressParallel<rapidgzip::ChunkDataCounter>(
            args, std::move( inputFile ), [&totalBytesRead] ( const auto& reader ) {
                totalBytesRead = reader->seek( 0, SEEK_END );
            } );
    } else {
        const auto readRange =
            [&totalBytesRead, hasOutputFiles, countLines, &writeAndCount]
            ( const auto&  reader,
              const size_t size )
            {
                /* An empty functor will lead to decompression to be skipped if the index is finalized! */
                if ( hasOutputFiles || countLines ) {
                    reader->read( writeAndCount, size );
                } else {
                    totalBytesRead += reader->read( /* do nothing */ nullptr, size );
                }
            };

        const auto readLines =
            [&] ( const auto&  reader,
                  const size_t lineCount,
                  const auto&  writeFunctor )
            {
                size_t lastLineOffset = reader->tell();
                const auto newlineCharacter = reader->newlineFormat() == NewlineFormat::LINE_FEED ? '\n' : '\r';
                auto remainingLineCount = lineCount;

                const auto forwardLineWrites =
                    [&lastLineOffset, &remainingLineCount, newlineCharacter, &writeFunctor]
                    ( const std::shared_ptr<rapidgzip::ChunkData>& chunkData,
                      const size_t                                 offsetInChunk,
                      const size_t                                 dataToWriteSize )
                    {
                        if ( remainingLineCount == 0 ) {
                            return;
                        }

                        using rapidgzip::deflate::DecodedData;
                        size_t nBytesRead{ 0 };
                        for ( auto it = DecodedData::Iterator( *chunkData, offsetInChunk, dataToWriteSize );
                              static_cast<bool>( it ); ++it )
                        {
                            const auto& [buffer, size] = *it;
                            const auto result = findNthNewline( { reinterpret_cast<const char*>( buffer ), size },
                                                                remainingLineCount, newlineCharacter );
                            remainingLineCount = result.remainingLineCount;
                            if ( remainingLineCount == 0 ) {
                                if ( result.position == std::string_view::npos ) {
                                    throw std::logic_error( "Find n-th line should return a valid position when "
                                                            "the input line count was not 0 but is 0 thereafter." );
                                }

                                /* Skip over last newline character with +1. */
                                nBytesRead += result.position + 1U;
                                lastLineOffset += result.position + 1U;
                                break;
                            }
                            nBytesRead += size;
                            lastLineOffset += size;
                        }

                        if ( nBytesRead > dataToWriteSize ) {
                            throw std::logic_error( "Shouldn't have read more bytes than specified in the chunk." );
                        }

                        writeFunctor( chunkData, offsetInChunk, nBytesRead );
                    };

                while ( remainingLineCount > 0 ) {
                    const auto currentBytesRead = reader->read( forwardLineWrites, 4_Mi );
                    if ( currentBytesRead == 0 ) {
                        break;
                    }
                }

                reader->seekTo( lastLineOffset );
            };

        errorCode = decompressParallel<rapidgzip::ChunkData>(
            args, std::move( inputFile ), [&] ( const auto& reader ) {
                if ( !args.fileRanges ) {
                    if ( !hasOutputFiles && countLines && !reader->newlineOffsets().empty() ) {
                        const auto& newlineOffset = reader->newlineOffsets().back();
                        newlineCount = newlineOffset.lineOffset;
                        totalBytesRead = reader->seekTo( newlineOffset.uncompressedOffsetInBytes );
                        /* Strictly speaking, we should be able to return here, but in case the newline offset
                         * semantics change or when only partial index should be loaded (not supported yet), then
                         * the subsequent "read until file end" should cover these cases. */
                    }
                    readRange( reader, std::numeric_limits<size_t>::max() );
                    return;
                }

                for ( const auto& range : *args.fileRanges ) {
                    if ( range.size == 0 ) {
                        continue;
                    }

                    if ( ( ( range.offsetIsLine && ( range.offset > 0 ) ) || range.sizeIsLine )
                         && !reader->newlineFormat() )
                    {
                        throw std::invalid_argument( "Currently, seeking and reading lines only works when "
                                                     "importing gztool indexes created with -x or -X!" );
                    }

                    /* Seek to line or byte offset. Note that line 0 starts at byte 0 by definition. */
                    if ( range.offsetIsLine && ( range.offset > 0 ) ) {
                        const auto& newlineOffsets = reader->newlineOffsets();
                        const auto lessLineOffset = [] ( const auto& a, const auto lineOffset ) {
                            return a.lineOffset < lineOffset;
                        };
                        auto newlineOffset = std::lower_bound( newlineOffsets.begin(), newlineOffsets.end(),
                                                               range.offset, lessLineOffset );
                        if ( newlineOffset == newlineOffsets.end() ) {
                            continue;
                        }

                        if ( newlineOffset == newlineOffsets.begin() ) {
                            throw std::logic_error( "Bisection may never point to the first element because "
                                                    "line offset 0 is always smaller than any line offset handled "
                                                    "in this branch!" );
                        }
                        --newlineOffset;

                        reader->seekTo( newlineOffset->uncompressedOffsetInBytes );

                        if ( newlineOffset->lineOffset >= range.offset ) {
                            throw std::logic_error( "Bisection should have returned a line offset prior to the "
                                                    "one we need to seek to." );
                        }

                        readLines( reader, range.offset - newlineOffset->lineOffset,
                                   [] ( const auto&, size_t, size_t ) {} );
                    } else {
                        reader->seekTo( range.offset );
                    }

                    if ( range.sizeIsLine ) {
                        readLines( reader, range.size, writeAndCount );
                    } else {
                        readRange( reader, range.size );
                    }
                }
            } );
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
        out << ( countBytes ? totalBytesRead : newlineCount ) << "\n";
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


#if !defined( WITH_PYTHON_SUPPORT ) && !defined( WITHOUT_MAIN )
int
main( int argc, char** argv )
{
    try
    {
        return rapidgzipCLI( argc, argv );
    }
    catch ( const gzip::BitReader::EndOfFileReached& exception )
    {
        std::cerr << "Unexpected end of file. Truncated or invalid gzip?\n";
        return 1;
    }
    catch ( const bzip2::BitReader::EndOfFileReached& exception )
    {
        std::cerr << "Unexpected end of file. Truncated or invalid bzip2?\n";
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
