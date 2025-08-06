#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <core/ParallelBitStringFinder.hpp>
#include <core/FileUtils.hpp>
#include <core/common.hpp>
#include <core/TestHelpers.hpp>
#include <filereader/Buffered.hpp>
#include <filereader/Standard.hpp>


using namespace rapidgzip;


template<class TemplatedBitStringFinder>
bool
testBitStringFinder( TemplatedBitStringFinder&& bitStringFinder,
                     const std::vector<size_t>& stringPositions )
{
    /* Gather all strings (time out at 1k strings because tests are written manually
     * and never will require that many matches, so somethings must have gone wrong */
    std::vector<size_t> matches;
    while ( true ) {
        matches.push_back( bitStringFinder.find() );
        if ( matches.back() == std::numeric_limits<size_t>::max() ) {
            matches.pop_back();
            break;
        }
    }

    ++gnTests;
    if ( matches != stringPositions ) {
        ++gnTestErrors;
        if ( matches.size() > 10 ) {
            std::cerr << "[FAIL] Matches with " << matches.size() << " elements != " << stringPositions << "\n";
        } else {
            std::cerr << "[FAIL] Matches: " << matches << " != " << stringPositions << "\n";
        }
        return false;
    }

    return true;
}


template<uint8_t bitStringSize>
void
testBitStringFinder( uint64_t                          bitStringToFind,
                     const std::vector<unsigned char>& buffer,
                     const std::vector<size_t>&        stringPositions )
{
    std::cerr << "Test finding bit string 0x" << std::hex << bitStringToFind << std::dec
              << " of size " << static_cast<int>( bitStringSize ) << " in buffer of size " << buffer.size() << " B\n";

    auto const * const rawBuffer = reinterpret_cast<const char*>( buffer.data() );

    for ( size_t parallelization = 1; parallelization <= std::thread::hardware_concurrency(); parallelization *= 2 )
    {
        /* test the version working on an input buffer */
        ParallelBitStringFinder<bitStringSize> bitStringFinder( rawBuffer,
                                                                buffer.size(),
                                                                bitStringToFind,
                                                                parallelization );
        if ( !testBitStringFinder( std::move( bitStringFinder ), stringPositions ) ) {
            std::cerr << "Version working on input buffer failed!\n";
        }
    }

    for ( size_t parallelization = 1; parallelization <= std::thread::hardware_concurrency(); parallelization *= 2 )
    {
        /* test the version working on an input file by writing the buffer to a temporary file */
        auto const file = make_unique_file_ptr( std::tmpfile() );  // NOLINT
        std::fwrite( buffer.data(), sizeof( buffer[0] ), buffer.size(), file.get() );
        /**
         * Flush the file so that BitReader sees the written data when accessing the file through the file descriptor.
         * Don't close file because:
         * > On some implementations (e.g. Linux), this function actually creates, opens, and immediately deletes
         * > the file from the file system
         * @see https://en.cppreference.com/w/cpp/io/c/tmpfile
         * Also, use smallest sane value for fileBufferSizeBytes = sizeof( uint64_t ) in order to check that
         * recognizing bit strings across file buffer borders works correctly.
         */
        std::fflush( file.get() );
        ParallelBitStringFinder<bitStringSize> bitStringFinder(
            std::make_unique<StandardFileReader>( fileno( file.get() ) ), bitStringToFind, parallelization
        );
        if ( !testBitStringFinder( std::move( bitStringFinder ), stringPositions ) ) {
            std::cerr << "Version working on input file failed!\n";
        }
    }
}


/* BitStringReader is optimized and only works with bit strings that are multiplies of and greater than 8. */
#if 0
/* Tests with single bit at subbit positions and a lot further away and definitely over the loading chunk size. */
void
testSingleBitAtChunkBoundary()
{
    std::vector<size_t> bitPositions;
    for ( size_t i = 0; i < 4; ++i ) {
        bitPositions.push_back( i );
        bitPositions.push_back( 4UL * 1024UL - 2 + i );
        bitPositions.push_back( 4UL * 1024UL * 8UL - 2 + i );
        bitPositions.push_back( 8UL * 1024UL * 8UL - 2 + i );
    }

    for ( const auto bitPosition : bitPositions ) {
        /* This size and a parallelization > 4 should give chunks sized 4096, 4096, 4096, 3712
         * because of the minimum chunk size. */
        std::vector<char> buffer( 16000, '\0' );
        reinterpret_cast<uint8_t&>( buffer[bitPosition / 8] ) = 1U << ( 7 - ( bitPosition % 8 ) );
        ParallelBitStringFinder<1> bitStringFinder( buffer.data(), buffer.size(), 0x01, 8 );
        if ( !testBitStringFinder( std::move( bitStringFinder ), { bitPosition } ) ) {
            std::cerr << "ParallelBitStringFinder failed for buffer sized " << buffer.size() << " with single bit "
                      << "at offset " << bitPosition << " b!\n";
        }
    }
}


/* Tests with single bit at subbit positions and a lot further away and definitely over file buffer size. */
void
testSingleBitAtFileBufferBoundary()
{
    std::vector<size_t> bitPositions;
    for ( size_t i = 0; i < 4; ++i ) {
        bitPositions.push_back( i );
        bitPositions.push_back( 1024UL * 1024UL * CHAR_BIT - 2 + i );
    }

    for ( const auto bitPosition : bitPositions ) {
        std::vector<char> buffer( 4'000'000, '\0' );
        reinterpret_cast<uint8_t&>( buffer[bitPosition / CHAR_BIT] ) = 1U << ( 7 - ( bitPosition % CHAR_BIT ) );
        BitStringFinder<1> bitStringFinder( std::make_unique<BufferedFileReader>( buffer ), 0x01, 8 );
        if ( !testBitStringFinder( std::move( bitStringFinder ), { bitPosition } ) ) {
            std::cerr << "ParallelBitStringFinder failed for buffer sized " << buffer.size() << " with single bit "
                      << "at offset " << bitPosition << " b!\n";
        }
    }
}
#endif


void
testSingleByteAtFileBufferBoundary()
{
    std::vector<size_t> bytePositions;
    for ( size_t i = 0; i < 4; ++i ) {
        bytePositions.push_back( i );
        bytePositions.push_back( 1_Mi - 2 + i );
        bytePositions.push_back( 2_Mi - 2 + i );
        bytePositions.push_back( 3_Mi - 2 + i );
    }

    for ( const auto bytePosition : bytePositions ) {
        std::vector<char> buffer( 4'000'000, '\0' );
        reinterpret_cast<uint8_t&>( buffer[bytePosition] ) = 0xFFU;
        ParallelBitStringFinder<CHAR_BIT> bitStringFinder( std::make_unique<BufferedFileReader>( buffer ), 0xFF, 8 );
        if ( !testBitStringFinder( std::move( bitStringFinder ), { bytePosition * CHAR_BIT } ) ) {
            std::cerr << "ParallelBitStringFinder failed for buffer sized " << buffer.size() << " with single bit "
                      << "at offset " << bytePosition << " b!\n";
        }
    }
}


int
main()
{
#ifndef SHORT_TESTS
    /* These tests take too long because the buffer is too large. */
    testSingleByteAtFileBufferBoundary();
    //testSingleBitAtFileBufferBoundary();
#endif

    //testSingleBitAtChunkBoundary();

    /* 0-size bit strings to find arguably makes no sense to test for. */
    //testBitStringFinder<0>( 0b0, {}, {} );
    //testBitStringFinder<0>( 0b0, { 0x00 }, {} );
    //testBitStringFinder<0>( 0b1111'1111, {}, {} );
    //testBitStringFinder<0>( 0b1111'1111, { 0x00 }, {} );

    /* BitStringReader is optimized and only works with bit strings that are multiplies of and greater than 8. */
    /*
    testBitStringFinder<1>( 0b0, { 0b0000'1111 }, { 0, 1, 2, 3 } );
    testBitStringFinder<1>( 0b0, { 0b1010'1010 }, { 1, 3, 5, 7 } );
    testBitStringFinder<1>( 0b0, { 0b1111'1111 }, {} );
    testBitStringFinder<1>( 0b0, { 0b0111'1111, 0b1111'1110 }, { 0, 15 } );
    testBitStringFinder<2>( 0b0, { 0b0000'1111 }, { 0, 1, 2 } );
    testBitStringFinder<3>( 0b0, { 0b0000'1111 }, { 0, 1 } );
    testBitStringFinder<4>( 0b0, { 0b0000'1111 }, { 0 } );
    testBitStringFinder<5>( 0b0, { 0b0000'1111 }, {} );

    testBitStringFinder<1>( 0b1111'1111, { 0b0000'1111 }, { 4, 5, 6, 7 } );
    testBitStringFinder<1>( 0b1111'1111, { 0b1010'1010 }, { 0, 2, 4, 6 } );
    testBitStringFinder<8>( 0b1111'1111, { 0b1111'1111 }, { 0 } );
    testBitStringFinder<1>( 0b1111'1111, { 0b1000'0000, 0b0000'0001 }, { 0, 15 } );
    testBitStringFinder<2>( 0b1111'1111, { 0b0000'1111 }, { 4, 5, 6 } );
    testBitStringFinder<3>( 0b1111'1111, { 0b0000'1111 }, { 4, 5 } );
    testBitStringFinder<4>( 0b1111'1111, { 0b0000'1111 }, { 4 } );
    testBitStringFinder<5>( 0b1111'1111, { 0b0000'1111 }, {} );

    testBitStringFinder<10>( 0b10'1010'1010, { 0b0101'0101, 0b0101'0101 }, { 1, 3, 5 } );
    */
    testBitStringFinder<48>( 0x314159265359ULL, { 0x11, 0x41, 0x59, 0x26, 0x53, 0x59 }, {} );
    testBitStringFinder<48>( 0x314159265359ULL, { 0x31, 0x41, 0x59, 0x26, 0x53, 0x58 }, {} );
    testBitStringFinder<48>( 0x314159265359ULL, { 0x31, 0x41, 0x59, 0x26, 0x53, 0x59 }, { 0 } );
    testBitStringFinder<48>( 0x314159265359ULL, { 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0, 0 }, { 0 } );
    testBitStringFinder<48>( 0x314159265359ULL, { 0, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0, 0 }, { 8 } );
    testBitStringFinder<48>( 0x314159265359ULL, { 0, 0, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0, 0 }, { 16 } );
    testBitStringFinder<48>( 0x314159265359ULL, { 0, 0, 0, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0, 0 }, { 24 } );
    testBitStringFinder<48>( 0x314159265359ULL, { 0, 0, 0, 0, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0, 0 }, { 32 } );

    testBitStringFinder<48>( 0x314159265359ULL, { 0x18, 0xA0, 0xAC, 0x93, 0x29, 0xAC, 0x80 }, { 1 } );
    testBitStringFinder<48>( 0x314159265359ULL, { 0x00, 0x62, 0x82, 0xB2, 0x4C, 0xA6, 0xB2 }, { 7 } );

    /* Tests with second match a lot further away and definitely over the loading chunk size. */
    {
        const std::vector<unsigned char> buffer = { 0, 0, 0, 0, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0, 0 };
        const std::vector<size_t> expectedResults = { 32 };

        const std::vector<char> secondMatchingString = { 0x31, 0x41, 0x59, 0x26, 0x53, 0x59 };
        auto const minSubChunkSize = 4096;
        /* At this offset the second sub chunk begins and it will actually become multi-threaded for small buffers. */
        auto const specialOffset = minSubChunkSize - buffer.size() - secondMatchingString.size();

        const std::vector<size_t> offsetsToTest = { 1, 100, 123, 1_Ki, 4_Ki - 1, 4_Ki, 28_Ki, 4_Mi,
                                                    specialOffset - 1, specialOffset, specialOffset + 1 };

        for ( const auto offset : offsetsToTest ) {
            auto tmpResults = expectedResults;
            tmpResults.push_back( ( buffer.size() + offset ) * CHAR_BIT );

            auto tmpBuf = buffer;
            tmpBuf.resize( tmpBuf.size() + offset, 0 );
            for ( auto c : secondMatchingString ) {
                tmpBuf.push_back( c );
            }

            testBitStringFinder<48>( 0x314159265359ULL, tmpBuf, tmpResults );
        }
    }

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors == 0 ? 0 : 1;
}
