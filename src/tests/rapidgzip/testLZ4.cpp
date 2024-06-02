#include <bit>
#include <bitset>
#include <iostream>
#include <string>

#include <core/common.hpp>
#include <core/FasterVector.hpp>
#include <core/FileUtils.hpp>
#include <core/TestHelpers.hpp>
#include <core/VectorView.hpp>
#include <filereader/Standard.hpp>

#include <lz4frame.h>


using namespace rapidgzip;


namespace xxhash32
{
static constexpr uint32_t PRIME32_1 = 0x9E3779B1U;  // 0b10011110001101110111100110110001
static constexpr uint32_t PRIME32_2 = 0x85EBCA77U;  // 0b10000101111010111100101001110111
static constexpr uint32_t PRIME32_3 = 0xC2B2AE3DU;  // 0b11000010101100101010111000111101
static constexpr uint32_t PRIME32_4 = 0x27D4EB2FU;  // 0b00100111110101001110101100101111
static constexpr uint32_t PRIME32_5 = 0x165667B1U;  // 0b00010110010101100110011110110001


[[nodiscard]] constexpr uint32_t
xxhash32Round( uint32_t acc,
               uint32_t lane )
{
    acc += lane * PRIME32_2;
    acc = std::rotl( acc, 13 );
    acc *= PRIME32_1;
    return acc;
}


[[nodiscard]] uint32_t
xxhash32( uint32_t seed = 0 )
{
    const auto acc1 = seed + PRIME32_1 + PRIME32_2;
    const auto acc2 = seed + PRIME32_2;
    const auto acc3 = seed + 0U;
    const auto acc4 = seed - PRIME32_1;

    return std::rotl( acc1, 1 ) + std::rotl( acc2, 7 ) + std::rotl( acc3, 12 ) + std::rotl( acc4, 18 );
}
}  // namespace xxhash32


/* https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md */
void
decodeLZ4Block( const VectorView<std::byte> toDecompress,
                FasterVector<std::byte>&    decompressed )
{
    decompressed.clear();

    size_t i{ 0 };
    while ( i < toDecompress.size() ) {
        const auto sequenceToken = static_cast<uint8_t>( toDecompress[i] );
        size_t length = ( sequenceToken & 0xF0U ) >> 4U;
        ++i;
        if ( length == 15 ) {
            while ( i < toDecompress.size() ) {
                const auto lengthToAdd = static_cast<uint8_t>( toDecompress[i] );
                ++i;
                const auto oldLength = length;
                length += lengthToAdd;
                if ( length < oldLength ) {
                    throw std::invalid_argument( "LZ4 block sequence length is too long and did overflow!" );
                }
                if ( lengthToAdd != 0xFFU ) {
                    break;
                }
            }
        }

        if ( i + length > toDecompress.size() ) {
            std::stringstream message;
            message << "LZ4 block sequence length " << length << " is larger than remaining data ("
                    << toDecompress.size() - i << ")! Currently at offset " << i << " out of " << toDecompress.size();
            throw std::invalid_argument( std::move( message ).str() );
        }
        decompressed.insert( decompressed.end(), toDecompress.begin() + i, toDecompress.begin() + i + length );
        i += length;

        /* The last block has no offset information! */
        if ( i >= toDecompress.size() ) {
            break;
        }

        if ( i + 2 >= toDecompress.size() ) {
            throw std::invalid_argument( "Premature end while trying to read the match offset!" );
        }
        const auto offset = static_cast<uint32_t>( static_cast<uint8_t>( toDecompress[i] ) )
                            + ( static_cast<uint32_t>( static_cast<uint8_t>( toDecompress[i + 1] ) ) << 8U );
        i += 2;
        if ( offset == 0 ) {
            throw std::invalid_argument( "Invalid match offset value 0 encountered at offset "
                                         + std::to_string( i ) + " out of " + std::to_string( toDecompress.size() )
                                         + "!" );
        }

        if ( offset > decompressed.size() ) {
            throw std::invalid_argument( "Offset pointing too far back!" );
        }
        const auto matchStart = decompressed.size() - offset;

        size_t matchLength = sequenceToken & 0x0FU;
        if ( matchLength == 15 ) {
            while ( i < toDecompress.size() ) {
                const auto lengthToAdd = static_cast<uint8_t>( toDecompress[i] );
                ++i;
                const auto oldLength = matchLength;
                matchLength += lengthToAdd;
                if ( matchLength < oldLength ) {
                    throw std::invalid_argument( "LZ4 block sequence length is too long and did overflow!" );
                }
                if ( lengthToAdd != 0xFFU ) {
                    break;
                }
            }
        }
        matchLength += 4;

        /* We need to ensure proper capacity to avoid iterator invalidation because we copy from itself! */
        if ( decompressed.size() + matchLength > decompressed.capacity() ) {
            decompressed.reserve( std::max( decompressed.capacity() * 2, decompressed.size() + matchLength ) );
        }
        while ( true ) {
            decompressed.insert( decompressed.end(), decompressed.begin() + matchStart,
                                 decompressed.begin() + matchStart + std::min<size_t>( matchLength, offset ) );
            if ( matchLength < offset ) {
                break;
            }
            matchLength -= offset;
        }
    }
}


void
decodeLZ4Custom( const std::filesystem::path& filePath )
{
    const auto tStartDecode = now();
    const auto file = std::make_unique<StandardFileReader>( filePath.string() );
    std::array<char, 4> magicBytes{};
    const auto magicBytesSize = file->read( magicBytes.data(), magicBytes.size() );

    std::cerr << std::hex;
    for ( const auto c : magicBytes ) {
        std::cerr << " " << (int)(uint8_t)c;
    }
    std::cerr << std::dec << "\n";

    /* https://github.com/lz4/lz4/blob/dev/doc/lz4_Frame_format.md
     * https://github.com/Cyan4973/xxHash/blob/dev/doc/xxhash_spec.md */
    static constexpr std::array<char, 4> MAGIC_BYTES = { 0x04, 0x22, 0x4D, 0x18 };
    if ( ( magicBytesSize != 4 ) || ( magicBytes != MAGIC_BYTES ) ) {
        std::cerr << "Magic byte mismatch!\n";
        return;
    }

    uint8_t frameFlagsBuffer{ 0 };
    if ( file->read( reinterpret_cast<char*>( &frameFlagsBuffer ), sizeof( frameFlagsBuffer ) ) != 1 ) {
        return;
    }
    std::bitset<8> frameFlags{ frameFlagsBuffer };

    if ( ( frameFlagsBuffer & 0b1100'0000U ) != 0b0100'0000U ) {
        std::cerr << "Frame version mismatch!\n";
        return;
    }

    if ( frameFlags[1] ) {
        std::cerr << "Reserved bit must be cleared\n";
        return;
    }

    const auto blockIndependence = frameFlags[5];
    const auto blockHasChecksum = frameFlags[4];
    const auto frameHasUncompressedSize = frameFlags[3];
    const auto frameHasChecksum = frameFlags[2];
    const auto frameHasDictionary = frameFlags[0];

    std::cerr << "Blocks independent: " << std::boolalpha << blockIndependence << "\n";
    std::cerr << "Blocks have checksum: " << std::boolalpha << blockHasChecksum << "\n";
    std::cerr << "Uncompressed frame size available: " << std::boolalpha << frameHasUncompressedSize << "\n";
    std::cerr << "Frame checksum available: " << std::boolalpha << frameFlags[2] << "\n";
    std::cerr << "Frame has dictionary: " << std::boolalpha << frameFlags[0] << "\n";

    uint8_t blockDescriptor{ 0 };
    if ( file->read( reinterpret_cast<char*>( &blockDescriptor ), sizeof( blockDescriptor ) ) != 1 ) {
        return;
    }

    if ( ( blockDescriptor & 0b1000'1111U ) != 0U ) {
        std::cerr << "Reserved bits in BD byte must be 0!\n";
        return;
    }

    const auto maximumBlockSizeID = ( blockDescriptor & 0b0111'0000U ) >> 4U;
    if ( ( maximumBlockSizeID < 4 ) || ( maximumBlockSizeID > 7 ) ) {
        std::cerr << "Invalid maximum block size (<64 KiB)!\n";
        return;
    }
    const auto maximumBlockSize = 256U << ( maximumBlockSizeID * 2U );

    std::cerr << "Maximum block size: " << formatBytes( maximumBlockSize ) << "\n";

    if ( frameHasUncompressedSize ) {
        uint64_t uncompressedSize{ 0 };
        if ( file->read( reinterpret_cast<char*>( &uncompressedSize ), sizeof( uncompressedSize ) )
             != sizeof( uncompressedSize ) )
        {
            std::cerr << "Unexpected end of file while reading uncompressed frame size!\n";
        }

        std::cerr << "Uncompressed frame size: " << formatBytes( uncompressedSize ) << "\n";
    }

    if ( frameHasDictionary ) {
        uint32_t dictionaryID{ 0 };
        if ( file->read( reinterpret_cast<char*>( &dictionaryID ), sizeof( dictionaryID ) )
             != sizeof( dictionaryID ) )
        {
            std::cerr << "Unexpected end of file while reading dictionary ID frame size!\n";
        }
    }

    uint8_t headerChecksum{ 0 };
    if ( file->read( reinterpret_cast<char*>( &headerChecksum ), sizeof( headerChecksum ) )
         != sizeof( headerChecksum ) )
    {
        std::cerr << "Unexpected end of file while reading header checksum!\n";
    }

    std::cerr << "\n";
    size_t totalBytesDecoded{ 0 };

    FasterVector<std::byte> buffer;
    FasterVector<std::byte> decompressed;

    while ( true ) {
        uint32_t blockSize{ 0 };
        if ( file->read( reinterpret_cast<char*>( &blockSize ), sizeof( blockSize ) )
             != sizeof( blockSize ) )
        {
            std::cerr << "Unexpected end of file while reading the block size!\n";
        }

        /* End of blocks marker. */
        if ( blockSize == 0 ) {
            break;
        }

        const auto blockIsUncompressed = ( blockSize & ( 1U << 31U ) ) != 0U;
        blockSize &= ~( 1U << 31U );

        //std::cerr << "Block size compressed: " << formatBytes( blockSize )
        //          << ". Is compressed: " << !blockIsUncompressed << "\n";

        if ( ( blockSize > maximumBlockSize ) && !blockIsUncompressed ) {
            std::cerr << "Only uncompressed blocks may be larger than the maximum block size!\n";
            return;
        }

        if ( blockIsUncompressed ) {
            file->seek( blockSize, SEEK_CUR );
        } else {
            buffer.resize( blockSize );
            file->read( reinterpret_cast<char*>( buffer.data() ), buffer.size() );
            decodeLZ4Block( buffer, decompressed );
            totalBytesDecoded += decompressed.size();
        }

        if ( blockHasChecksum ) {
            uint32_t blockChecksum{ 0 };
            if ( file->read( reinterpret_cast<char*>( &blockChecksum ), sizeof( blockChecksum ) )
                 != sizeof( blockChecksum ) )
            {
                std::cerr << "Unexpected end of file while reading block checksum!\n";
            }
        }
    }

    std::cerr << "Decoded in total: " << formatBytes( totalBytesDecoded ) << " (" << totalBytesDecoded << " B)\n";
    std::cerr << "Took " << duration( tStartDecode ) << " s -> "
              << formatBytes( totalBytesDecoded / duration( tStartDecode ) ) << "/s\n";
}


void
decodeLZ4( const std::filesystem::path& filePath )
{
    const auto tStartDecode = now();
    const auto file = std::make_unique<StandardFileReader>( filePath.string() );

    FasterVector<std::byte> buffer;
    FasterVector<std::byte> decompressed( 4_Mi );
    size_t totalBytesDecoded{ 0 };

    /** @todo wrap LZ4F_freeDecompressionContext in RAII */
    LZ4F_dctx* context{ nullptr };
    if ( const auto result = LZ4F_createDecompressionContext( &context, LZ4F_VERSION ); LZ4F_isError( result ) ) {
        throw std::runtime_error( "Failed to create LZ4 decompression context!" );
    }

    size_t bufferConsumed{ 0 };
    while ( true ) {
        if ( bufferConsumed >= buffer.size() ) {
            buffer.resize( 4_Mi );
            buffer.resize( file->read( reinterpret_cast<char*>( buffer.data() ), buffer.size() ) );
            bufferConsumed = 0;
            if ( buffer.empty() ) {
                break;
            }
        }

        size_t bufferRemaining = buffer.size() - bufferConsumed;
        size_t decompressedSize = decompressed.size();
        /* Returns an error code or an estimated of the expected bytes to be consumed in the next call.
         * If it expects 0 to be consumed, the end (of the frame) has been reached. */
        const auto result = LZ4F_decompress( context, decompressed.data(), &decompressedSize,
                                             buffer.data() + bufferConsumed, &bufferRemaining, nullptr );
        if ( LZ4F_isError( result ) ) {
            LZ4F_freeDecompressionContext( context );
            throw std::runtime_error( "LZ4 decompression failed: " + std::string( LZ4F_getErrorName( result ) ) );
        }
        std::cerr << "Decompressed " << formatBytes( decompressedSize ) << " and read "
                  << formatBytes( bufferRemaining ) << "\n";
        bufferConsumed += bufferRemaining;
        totalBytesDecoded += decompressedSize;
    }

    LZ4F_freeDecompressionContext( context );

    std::cerr << "Decoded in total: " << formatBytes( totalBytesDecoded ) << " (" << totalBytesDecoded << " B)\n";
    std::cerr << "Took " << duration( tStartDecode ) << " s -> "
              << formatBytes( totalBytesDecoded / duration( tStartDecode ) ) << "/s\n";
}


int
main( int    argc,
      char** argv )
{
    if ( argc == 0 ) {
        std::cerr << "Expected at least the launch command as the first argument!\n";
        return 1;
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

    decodeLZ4( testsFolder / "random-128KiB.lz4" );
    /* @todo */
    //decodeLZ4Custom( "silesia.tar.lz4" );
    //decodeLZ4( "silesia.tar.lz4" );

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors == 0 ? 0 : 1;
}
