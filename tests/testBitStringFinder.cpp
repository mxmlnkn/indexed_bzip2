#include <cassert>
#include <cstdio>
#include <iostream>

#include <unistd.h>

#include "BitStringFinder.hpp"


namespace {
int gnTests = 0;
int gnTestErrors = 0;


#if 0
template<typename T>
std::ostream&
operator<<( std::ostream&  out,
            std::vector<T> vector )
{
    out << "{ ";
    for ( const auto value : vector ) {
        out << value << ", ";
    }
    out << " }";
    return out;
}
#endif
}


void
testBitStringFinder( BitStringFinder&& bitStringFinder,
                     const std::vector<size_t>& stringPositions )
{
    /* Gather all strings (time out at 1k strings because tests are written manually
     * and never will require that many matches, so somethings must have gone wrong */
    std::vector<size_t> matches;
    for ( int i = 0; i < 16; ++i ) {
        matches.push_back( bitStringFinder.find() );
        if ( matches.back() == std::numeric_limits<size_t>::max() ) {
            matches.pop_back();
            break;
        }
    }

    ++gnTests;
    if ( matches != stringPositions ) {
        ++gnTestErrors;
        std::cerr << "[FAIL] Matches: " << matches << " != " << stringPositions << "\n";
    }
}


void
testBitStringFinder( uint64_t                          bitStringToFind,
                     uint8_t                           bitStringSize,
                     const std::vector<unsigned char>& buffer,
                     const std::vector<size_t>&        stringPositions )
{
    const auto rawBuffer = reinterpret_cast<const char*>( buffer.data() );

    {
        /* test the version working on an input buffer */
        BitStringFinder bitStringFinder( rawBuffer, buffer.size(), bitStringToFind, bitStringSize );
        testBitStringFinder( std::move( bitStringFinder ), stringPositions );
    }
    {
        /* test the version working on an input file by writing the buffer to a temporary file */
        const auto file = std::tmpfile();
        const auto nWritten = std::fwrite( buffer.data(), sizeof( buffer[0] ), buffer.size(), file );
        std::cerr << "wrote " << buffer.size() << " bytes\n";
        /**
         * Flush the file so that BitReader sees the written data when accessing the file through the file descriptor.
         * Don't close file because:
         * > On some implementations (e.g. Linux), this function actually creates, opens, and immediately deletes
         * > the file from the file system
         * @see https://en.cppreference.com/w/cpp/io/c/tmpfile
         * Also, use smallest sane value for fileBufferSizeBytes = sizeof( uint64_t ) in order to check that
         * recognizing bit strings accross file buffer borders works correctly.
         */
        std::fflush( file );
        BitStringFinder bitStringFinder( fileno( file ), bitStringToFind, bitStringSize, sizeof( uint64_t ) );
        testBitStringFinder( std::move( bitStringFinder ), stringPositions );
        std::fclose( file );
    }
}


int
main( void )
{
    testBitStringFinder( 0b0, 0, {}, {} );
    testBitStringFinder( 0b0, 0, { 0x00 }, {} );
    testBitStringFinder( 0b0, 1, { 0b0000'1111 }, { 0, 1, 2, 3 } );
    testBitStringFinder( 0b0, 1, { 0b1010'1010 }, { 1, 3, 5, 7 } );
    testBitStringFinder( 0b0, 1, { 0b1111'1111 }, {} );
    testBitStringFinder( 0b0, 1, { 0b0111'1111, 0b1111'1110 }, { 0, 15 } );
    testBitStringFinder( 0b0, 2, { 0b0000'1111 }, { 0, 1, 2 } );
    testBitStringFinder( 0b0, 3, { 0b0000'1111 }, { 0, 1 } );
    testBitStringFinder( 0b0, 4, { 0b0000'1111 }, { 0 } );
    testBitStringFinder( 0b0, 5, { 0b0000'1111 }, {} );

    testBitStringFinder( 0b1111'1111, 0, {}, {} );
    testBitStringFinder( 0b1111'1111, 0, { 0x00 }, {} );
    testBitStringFinder( 0b1111'1111, 1, { 0b0000'1111 }, { 4, 5, 6, 7 } );
    testBitStringFinder( 0b1111'1111, 1, { 0b1010'1010 }, { 0, 2, 4, 6 } );
    testBitStringFinder( 0b1111'1111, 8, { 0b1111'1111 }, { 0 } );
    testBitStringFinder( 0b1111'1111, 1, { 0b1000'0000, 0b0000'0001 }, { 0, 15 } );
    testBitStringFinder( 0b1111'1111, 2, { 0b0000'1111 }, { 4, 5, 6 } );
    testBitStringFinder( 0b1111'1111, 3, { 0b0000'1111 }, { 4, 5 } );
    testBitStringFinder( 0b1111'1111, 4, { 0b0000'1111 }, { 4 } );
    testBitStringFinder( 0b1111'1111, 5, { 0b0000'1111 }, {} );

    testBitStringFinder( 0b10'1010'1010, 10, { 0b0101'0101, 0b0101'0101 }, { 1, 3, 5 } );
    testBitStringFinder( 0x314159265359ULL, 48, { 0x11, 0x41, 0x59, 0x26, 0x53, 0x59 }, {} );
    testBitStringFinder( 0x314159265359ULL, 48, { 0x31, 0x41, 0x59, 0x26, 0x53, 0x58 }, {} );
    testBitStringFinder( 0x314159265359ULL, 48, { 0x31, 0x41, 0x59, 0x26, 0x53, 0x59 }, { 0 } );
    testBitStringFinder( 0x314159265359ULL, 48, { 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0, 0 }, { 0 } );
    testBitStringFinder( 0x314159265359ULL, 48, { 0, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0, 0 }, { 8 } );
    testBitStringFinder( 0x314159265359ULL, 48, { 0, 0, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0, 0 }, { 16 } );
    testBitStringFinder( 0x314159265359ULL, 48, { 0, 0, 0, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0, 0 }, { 24 } );
    testBitStringFinder( 0x314159265359ULL, 48, { 0, 0, 0, 0, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0, 0 }, { 32 } );

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors;
}
