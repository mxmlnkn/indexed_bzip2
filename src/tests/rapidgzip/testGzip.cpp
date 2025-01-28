#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <common.hpp>
#include <filereader/Buffered.hpp>
#include <filereader/Standard.hpp>
#include <GzipReader.hpp>
#include <rapidgzip.hpp>
#include <TestHelpers.hpp>


using namespace rapidgzip;


// *INDENT-OFF*
const std::vector<uint8_t> NANO_SAMPLE_GZIP = {
    /*          ID1   ID2   CM    FLG  [       MTIME        ]     XFL   OS   [      FNAME = "nano"      ]  <Deflate */
    /* 0x00 */ 0x1F, 0x8B, 0x08, 0x08, 0xF5, 0x04, 0xDB, 0x61,   0x02, 0x03, 0x6E, 0x61, 0x6E, 0x6F, 0x00, 0x05,
    /* 0x10 */ 0xC1, 0xDD, 0x0E, 0x82, 0x20, 0x18, 0x00, 0xD0,   0xFB, 0x5E, 0x46, 0x92, 0x50, 0xB9, 0x94, 0xD8,
    /* 0x20 */ 0x6A, 0x96, 0x21, 0xD6, 0x4C, 0xB9, 0x54, 0xF4,   0x63, 0xFE, 0xA4, 0x86, 0x6E, 0xE6, 0xD3, 0x77,
    /* 0x30 */ 0x8E, 0xC5, 0x42, 0x51, 0x3C, 0xE8, 0xF9, 0x54,   0x7D, 0xD6, 0x46, 0x54, 0x04, 0xD6, 0x6F, 0x8A,
    /* 0x40 */ 0xB4, 0xF4, 0xB9, 0xF3, 0xCE, 0xAE, 0x2C, 0xB7,   0x2F, 0xD0, 0xA1, 0xB7, 0xA3, 0xA6, 0xD8, 0xF9,
    /* 0x50 */ 0xE5, 0x9C, 0x73, 0xE8, 0xEB, 0x3B, 0xA2, 0xDB,   0xE4, 0x2C, 0x95, 0xFB, 0xF4, 0xB2, 0x36, 0xC2,
    /* 0x60 */ 0xC7, 0x64, 0x54, 0x3F, 0x30, 0x2C, 0xE9, 0x0F,   0x6A, 0xD1, 0x4A, 0x78, 0x13, 0xD9, 0xAC, 0x0F,
    /* 0x70 */ 0xB4, 0x78, 0x0C, 0x36, 0x66, 0x8A, 0xDA, 0xA0,   0x93, 0xB3, 0xCB, 0x6E, 0x6E, 0x4D, 0xB8, 0x09,
    /* 0x80 */ 0xF1, 0x18, 0xB5, 0x25, 0xC3, 0x32, 0x8D, 0x7D,   0x30, 0x41, 0x47, 0xFE, 0x36, 0xC3, 0xC5, 0x28,
    /* 0x90 */ 0x80, 0x00, 0x00, 0x00
};
// *INDENT-ON*


const std::string_view NANO_SAMPLE_DECODED{
    "s3OZ93mdq4cnufOc5gurR0dQ7D/WVHBXsTgdA6z0fYzDGCXDgleL09xp/tc2S6VjJ31PoZyghBPl\n"
    "ZtdZO6p5xs7g9YNmsMBZ9s8kQq2BK2e5DhA3oJjbB3QRM7gh8k5"
};


[[nodiscard]] std::pair<std::vector<char>, std::vector<char> >
duplicateNanoStream( size_t multiples )
{
    std::vector<char> encoded( NANO_SAMPLE_GZIP.size() * multiples );
    for ( size_t i = 0; i < multiples; ++i ) {
        std::copy( NANO_SAMPLE_GZIP.begin(), NANO_SAMPLE_GZIP.end(),
                   encoded.begin() + static_cast<std::ptrdiff_t>( i * NANO_SAMPLE_GZIP.size() ) );
    }

    std::vector<char> decoded( NANO_SAMPLE_DECODED.size() * multiples );
    for ( size_t i = 0; i < multiples; ++i ) {
        std::copy( NANO_SAMPLE_DECODED.begin(), NANO_SAMPLE_DECODED.end(),
                   decoded.begin() + static_cast<std::ptrdiff_t>( i * NANO_SAMPLE_DECODED.size() ) );
    }

    return { encoded, decoded };
}


void
testSerialDecoderNanoSample()
{
    const std::vector<char> signedData( NANO_SAMPLE_GZIP.begin(), NANO_SAMPLE_GZIP.end() );
    GzipReader gzipReader( std::make_unique<BufferedFileReader>( signedData ) );
    gzipReader.setCRC32Enabled( true );

    std::vector<char> result( NANO_SAMPLE_DECODED.size() + 10, 0 );
    const auto nBytesDecoded = gzipReader.read( -1, result.data(), result.size() );
    REQUIRE( nBytesDecoded == NANO_SAMPLE_DECODED.size() );
    REQUIRE( std::equal( NANO_SAMPLE_DECODED.begin(), NANO_SAMPLE_DECODED.end(), result.begin() ) );
}


void
testSerialDecoderNanoSample( size_t multiples,
                             size_t bufferSize )
{
    const auto [encoded, decoded] = duplicateNanoStream( multiples );

    GzipReader gzipReader( std::make_unique<BufferedFileReader>( encoded ) );
    gzipReader.setCRC32Enabled( true );

    std::vector<char> result( bufferSize, 0 );
    size_t totalBytesDecoded = 0;
    while ( !gzipReader.eof() ) {
        const auto nBytesDecoded = gzipReader.read( -1, result.data(), result.size() );
        if ( nBytesDecoded < result.size() ) {
            REQUIRE( nBytesDecoded == ( decoded.size() % bufferSize ) );
        }
        REQUIRE( std::equal( result.begin(), result.begin() + nBytesDecoded, decoded.begin() + totalBytesDecoded ) );
        totalBytesDecoded += nBytesDecoded;
    }
}


void
testSerialDecoderNanoSampleStoppingPoints()
{
    const auto multiples = 2;
    const auto [encoded, decoded] = duplicateNanoStream( multiples );

    const auto collectStoppingPoints =
        [&encoded = encoded, &decoded = decoded] ( StoppingPoint stoppingPoint )
        {
            std::vector<size_t> offsets;
            std::vector<size_t> compressedOffsets;

            GzipReader gzipReader( std::make_unique<BufferedFileReader>( encoded ) );
            gzipReader.setCRC32Enabled( true );

            std::vector<char> result( decoded.size(), 0 );
            size_t totalBytesDecoded = 0;
            while ( !gzipReader.eof() ) {
                const auto nBytesDecoded = gzipReader.read( -1, result.data(), result.size(), stoppingPoint );
                REQUIRE( std::equal( result.begin(), result.begin() + nBytesDecoded,
                                     decoded.begin() + totalBytesDecoded ) );
                totalBytesDecoded += nBytesDecoded;

                offsets.emplace_back( gzipReader.tell() );
                compressedOffsets.emplace_back( gzipReader.tellCompressed() );
            }

            return std::make_pair( offsets, compressedOffsets );
        };

    {
        const auto [offsets, compressedOffsets] = collectStoppingPoints( StoppingPoint::NONE );
        REQUIRE( offsets == std::vector<size_t>( { decoded.size() } ) );
        REQUIRE( compressedOffsets == std::vector<size_t>( { encoded.size() * 8 } ) );
    }

    {
        const auto [offsets, compressedOffsets] = collectStoppingPoints( StoppingPoint::END_OF_STREAM );
        REQUIRE( offsets == std::vector<size_t>( { NANO_SAMPLE_DECODED.size(), decoded.size() } ) );
        REQUIRE( compressedOffsets == std::vector<size_t>( { NANO_SAMPLE_GZIP.size() * 8, encoded.size() * 8 } ) );
    }

    {
        const auto [offsets, compressedOffsets] =
            collectStoppingPoints( StoppingPoint::END_OF_STREAM_HEADER );
        REQUIRE( offsets == std::vector<size_t>( { 0, NANO_SAMPLE_DECODED.size(), decoded.size() } ) );
        REQUIRE( compressedOffsets == std::vector<size_t>( { 15UL * 8UL, ( NANO_SAMPLE_GZIP.size() + 15 ) * 8,
                                                             encoded.size() * 8 } ) );
    }

    {
        const auto [offsets, compressedOffsets] =
            collectStoppingPoints( StoppingPoint::END_OF_BLOCK_HEADER );
        REQUIRE( offsets == std::vector<size_t>( { 0, NANO_SAMPLE_DECODED.size(), decoded.size() } ) );
        REQUIRE( compressedOffsets == std::vector<size_t>( { 15 * 8 + 270, ( NANO_SAMPLE_GZIP.size() + 15 ) * 8 + 270,
                                                             encoded.size() * 8 } ) );
    }

    {
        const auto [offsets, compressedOffsets] =
            collectStoppingPoints( StoppingPoint::END_OF_BLOCK );
        REQUIRE( offsets == std::vector<size_t>( { NANO_SAMPLE_DECODED.size(), decoded.size(), decoded.size() } ) );
        constexpr auto FOOTER_SIZE = 8;
        REQUIRE( compressedOffsets == std::vector<size_t>( { ( NANO_SAMPLE_GZIP.size() - FOOTER_SIZE ) * 8,
                                                             ( encoded.size() - FOOTER_SIZE ) * 8,
                                                             encoded.size() * 8 } ) );
    }
}


void
testSerialDecoder( std::filesystem::path const& decodedFilePath,
                   std::filesystem::path const& encodedFilePath,
                   size_t                const  bufferSize )
{
    std::vector<char> decodedBuffer( bufferSize );
    std::vector<char> buffer( bufferSize );

    std::ifstream decodedFile( decodedFilePath );
    GzipReader gzipReader( std::make_unique<StandardFileReader>( encodedFilePath ) );
    gzipReader.setCRC32Enabled( true );

    size_t totalBytesDecoded{ 0 };
    while ( !gzipReader.eof() ) {
        const auto nBytesRead = gzipReader.read( -1, buffer.data(), buffer.size() );
        buffer.resize( nBytesRead );
        if ( nBytesRead == 0 ) {
            REQUIRE( gzipReader.eof() );
            break;
        }

        /* Compare with ground truth. */
        decodedBuffer.resize( buffer.size() );
        decodedFile.read( decodedBuffer.data(), static_cast<std::ptrdiff_t>( decodedBuffer.size() ) );
        REQUIRE( !decodedFile.fail() );
        REQUIRE( static_cast<size_t>( decodedFile.gcount() ) == buffer.size() );

        REQUIRE( std::equal( buffer.begin(), buffer.end(), decodedBuffer.begin() ) );
        if ( !std::equal( buffer.begin(), buffer.end(), decodedBuffer.begin() ) ) {
            for ( size_t i = 0; i < buffer.size(); ++i ) {
                if ( buffer[i] != decodedBuffer[i] ) {
                    std::cerr << "Decoded contents differ at position " << i << " B: "
                              << buffer[i] << " != " << decodedBuffer[i] << " ("
                              << (int)buffer[i] << " != " << (int)decodedBuffer[i] << ") while decoding "
                              << decodedFilePath << " with buffer size " << bufferSize << "\n";
                    break;
                }
            }
        }

        totalBytesDecoded += buffer.size();
    }

    REQUIRE( totalBytesDecoded == fileSize( decodedFilePath ) );
    std::cerr << "Decoded " << decodedFilePath.filename() << " with buffer size " << bufferSize << "\n";
}


void
testTwoStagedDecoding( const std::string& encodedFilePath,
                       const std::string& decodedFilePath )
{
    /* Read first deflate block so that we can try decoding from the second one. */
    GzipReader gzipReader{ std::make_unique<StandardFileReader>( encodedFilePath ) };
    gzipReader.setCRC32Enabled( true );
    std::vector<char> decompressed( 1_Mi );
    const auto firstBlockSize = gzipReader.read( -1, decompressed.data(), decompressed.size(),
                                                 StoppingPoint::END_OF_BLOCK );
    decompressed.resize( firstBlockSize );
    REQUIRE( gzipReader.currentPoint() == StoppingPoint::END_OF_BLOCK );

    /* Save all information required for seeking directly to second block. */
    const auto secondBlockOffset = gzipReader.tellCompressed();
    std::array<std::uint8_t, deflate::MAX_WINDOW_SIZE> lastWindow{};
    const auto sizeToCopy = std::min( lastWindow.size(), decompressed.size() );
    std::memcpy( lastWindow.data() + lastWindow.size() - sizeToCopy,
                 decompressed.data() + decompressed.size() - sizeToCopy,
                 sizeToCopy );

    gzipReader.read( -1, nullptr, std::numeric_limits<size_t>::max(), StoppingPoint::ALL );
    if ( gzipReader.currentPoint() != StoppingPoint::END_OF_BLOCK_HEADER ) {
        /* Ignore files with only one block for this test. */
        return;
    }

    std::cout << "Test two-staged decoding for " << encodedFilePath << "\n";

    /* Check that decompressed data and the last window match the ground truth. */
    std::ifstream decodedFile( decodedFilePath );
    std::vector<char> uncompressed( decompressed.size() );
    decodedFile.read( uncompressed.data(), static_cast<std::ptrdiff_t>( uncompressed.size() ) );
    REQUIRE( std::equal( decompressed.begin(), decompressed.end(), uncompressed.begin() ) );

    const auto validWindowSize = std::min( lastWindow.size(), firstBlockSize );
    const auto validWindowMatches = std::equal(
        lastWindow.end() - validWindowSize, lastWindow.end(),
        uncompressed.end() - static_cast<std::ptrdiff_t>( validWindowSize ),
        [] ( auto a, auto b ) { return a == static_cast<decltype( a )>( b ); } );
    REQUIRE( validWindowMatches );
    if ( !validWindowMatches ) {
        for ( size_t i = lastWindow.size() - validWindowSize; i < lastWindow.size(); ++i ) {
            const auto correct = uncompressed[uncompressed.size() - validWindowSize + i];
            const auto decoded = lastWindow.at( i );
            if ( static_cast<char>( decoded ) != correct ) {
                std::cerr << "Decoded contents differ at position " << i << " B: "
                          << decoded << " != " << correct << " ("
                          << (int)decoded << " != " << (int)correct << ") (lastWindow != file)\n";
                break;
            }
        }
    }

    /* Note that CRC32 won't work anyway when there are marker bytes! */
    using DeflateBlock = rapidgzip::deflate::Block</* CRC32 */ false>;

    /* Try reading, starting from second block. */
    gzip::BitReader bitReader{ std::make_unique<StandardFileReader>( encodedFilePath ) };
    bitReader.seek( static_cast<long long int>( secondBlockOffset ) );
    DeflateBlock block;

    {
        const auto error = block.readHeader( bitReader );
        REQUIRE( error == Error::NONE );
    }

    const auto [bufferViews, error] = block.read( bitReader, std::numeric_limits<size_t>::max() );
    REQUIRE( error == Error::NONE );

    deflate::DecodedData decodedData;
    decodedData.append( bufferViews );
    decodedData.applyWindow( { lastWindow.data(), lastWindow.size() } );

    std::vector<uint8_t> concatenated;
    for ( auto it = deflate::DecodedData::Iterator( decodedData ); static_cast<bool>( it ); ++it ) {
        const auto [buffer, size] = *it;
        const auto* const ubuffer = reinterpret_cast<const uint8_t*>( buffer );
        concatenated.insert( concatenated.end(), ubuffer, ubuffer + size );
    }

    /* Compare concatenated result. */
    std::vector<uint8_t> decodedBuffer( 1_Mi );
    REQUIRE( decodedBuffer.size() >= concatenated.size() );
    decodedFile.seekg( static_cast<long long int>( firstBlockSize ) );
    decodedFile.read( reinterpret_cast<char*>( decodedBuffer.data() ),
                      static_cast<long long int>( concatenated.size() ) );

    REQUIRE( std::equal( concatenated.begin(), concatenated.end(), decodedBuffer.begin() ) );
    if ( !std::equal( concatenated.begin(), concatenated.end(), decodedBuffer.begin() ) ) {
        for ( size_t i = 0; i < concatenated.size(); ++i ) {
            if ( concatenated[i] != decodedBuffer[i] ) {
                std::cerr << "Decoded contents differ at position " << i << " B: "
                          << concatenated[i] << " != " << decodedBuffer[i] << " ("
                          << (int)concatenated[i] << " != " << (int)decodedBuffer[i] << ") (concatenated != file)\n";
                break;
            }
        }
    }

    /* Replace marker bytes inside the block itself. */
    block.setInitialWindow( { lastWindow.data(), lastWindow.size() } );
}


int
main( int    argc,
      char** argv )
{
    if ( argc == 0 ) {
        std::cerr << "Expected at least the launch command as the first argument!\n";
        return 1;
    }

    testSerialDecoderNanoSampleStoppingPoints();
    testSerialDecoderNanoSample();
    for ( const auto multiples : { 1, 2, 3, 10 } ) {
        for ( const auto bufferSize : std::vector<size_t>{ 1, 2, 3, 4, 12, 32, 300, 1_Ki, 1_Mi } ) {
            std::cerr << "Try to decode " << multiples << " nano samples with buffer size: " << bufferSize << "\n";
            testSerialDecoderNanoSample( multiples, bufferSize );
        }
    }

    const std::string binaryFilePath( argv[0] );
    std::string binaryFolder = ".";
    if ( const auto lastSlash = binaryFilePath.find_last_of( '/' ); lastSlash != std::string::npos ) {
        binaryFolder = std::string( binaryFilePath.begin(),
                                    binaryFilePath.begin() + static_cast<std::string::difference_type>( lastSlash ) );
    }
    const auto testsFolder =
        static_cast<std::filesystem::path>(
            findParentFolderContaining( binaryFolder, "src/tests/data/base64-256KiB.bgz" )
        ) / "src" / "tests" / "data";

    for ( auto const& entry : std::filesystem::directory_iterator( testsFolder ) ) {
        if ( !entry.is_regular_file() || !entry.path().has_extension() ) {
            continue;
        }

        const auto extension = entry.path().extension().string();
        const std::unordered_set<std::string> validExtensions = { ".gz", ".bgz", ".igz", ".pigz" };
        if ( validExtensions.find( extension ) == validExtensions.end() ) {
            continue;
        }

        const auto& encodedFilePath = entry.path();
        auto decodedFilePath = entry.path();
        decodedFilePath.replace_extension( "" );
        if ( !std::filesystem::exists( decodedFilePath ) ) {
            continue;
        }

        for ( const auto bufferSize : std::vector<size_t>{ 1, 2, 12, 32, 1000, 1_Ki, 128_Ki, 1_Mi, 64_Mi } ) {
            try {
                testSerialDecoder( decodedFilePath.string(), encodedFilePath.string(), bufferSize );
            } catch ( const std::exception& e ) {
                std::cerr << "Exception was thrown: " << e.what() << "\n";
                REQUIRE( false );
            }
        }

        testTwoStagedDecoding( encodedFilePath.string(), decodedFilePath.string() );
    }

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors == 0 ? 0 : 1;
}
