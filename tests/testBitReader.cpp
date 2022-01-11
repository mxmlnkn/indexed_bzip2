#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <common.hpp>
#include <BitReader.hpp>
#include <BufferedFileReader.hpp>


template<uint8_t bitsSet>
uint32_t nLowestBitsSet32()
{
    return nLowestBitsSet<uint32_t, bitsSet>();
}

template<uint8_t bitsSet>
uint64_t nLowestBitsSet64()
{
    return nLowestBitsSet<uint64_t, bitsSet>();
}


void
testMSBBitReader()
{
    const std::vector<char> fileContents ={
        /*       0x5A                0xAA               0x0F               0x0F               0x0F */
        (char)0b0101'1010, (char)0b1010'1010, (char)0b0000'1111, (char)0b0000'1111, (char)0b0000'1111
    };
    BitReader<true> bitReader( std::make_unique<BufferedFileReader>( fileContents ) );

    REQUIRE( bitReader.read<0>() == 0b0 );
    REQUIRE( bitReader.read<1>() == 0b0 );
    REQUIRE( bitReader.tell() == 1 );
    REQUIRE( bitReader.read<1>() == 0b1 );
    REQUIRE( bitReader.tell() == 2 );
    REQUIRE( bitReader.read<2>() == 0b01 );
    REQUIRE( bitReader.tell() == 4 );
    REQUIRE( bitReader.read<4>() == 0b1010 );
    REQUIRE( bitReader.tell() == 8 );
    REQUIRE( bitReader.read<8>() == 0b1010'1010 );
    REQUIRE( bitReader.tell() == 16 );
    REQUIRE( bitReader.read<8>() == 0b0000'1111 );
    REQUIRE( bitReader.tell() == 24 );

    REQUIRE( bitReader.seek( 0, SEEK_SET ) == 0 );
    REQUIRE( bitReader.tell() == 0 );
    REQUIRE( bitReader.read<8>() == 0b0101'1010 );
    REQUIRE( bitReader.tell() == 8 );
    REQUIRE( bitReader.read<16>() == 0b1010'1010'0000'1111 );
    REQUIRE( bitReader.tell() == 24 );

    REQUIRE( bitReader.seek( -8, SEEK_CUR ) == 16 );
    REQUIRE( bitReader.tell() == 16 );
    REQUIRE( bitReader.read<8>() == 0b0000'1111 );
    REQUIRE( bitReader.tell() == 24 );

    REQUIRE( bitReader.seek( -8, SEEK_END ) == 32 );
    REQUIRE( bitReader.read<1>() == 0b0 );
    REQUIRE( bitReader.tell() == 33 );
    REQUIRE( bitReader.read<3>() == 0b000 );
    REQUIRE( bitReader.tell() == 36 );
    REQUIRE( bitReader.read<4>() == 0b1111 );
    REQUIRE( bitReader.tell() == 40 );
    REQUIRE( bitReader.eof() );

    REQUIRE( bitReader.seek( 0, SEEK_SET ) == 0 );
    REQUIRE( bitReader.read<24>() == 0x5AAA0F );
    REQUIRE( bitReader.tell() == 24 );

    REQUIRE( bitReader.seek( 0, SEEK_SET ) == 0 );
    REQUIRE( bitReader.read<32>() == 0x5AAA'0F0F );
    REQUIRE( bitReader.tell() == 32 );

    REQUIRE( bitReader.seek( 4, SEEK_SET ) == 4 );
    REQUIRE( bitReader.read<13>() == 0b1010'1010'1010'0 );
    REQUIRE( bitReader.tell() == 17 );

    REQUIRE( bitReader.seek( 4, SEEK_SET ) == 4 );
    REQUIRE( bitReader.read<32>() == 0xAAA0'F0F0 );
    REQUIRE( bitReader.tell() == 36 );
    REQUIRE( bitReader.read<2>() == 0b11 );
    REQUIRE( bitReader.read<2>() == 0b11 );
    REQUIRE( bitReader.tell() == 40 );
    REQUIRE( bitReader.eof() );

    REQUIRE( bitReader.seek( -35, SEEK_END ) == 5 );
    REQUIRE( bitReader.tell() == 5 );
    REQUIRE( bitReader.read<32>() == 0b010'1010'1010'0000'1111'0000'1111'0000'1 );
    REQUIRE( bitReader.tell() == 37 );
}


void
testLSBBitReader()
{
    const std::vector<char> fileContents ={
        /*       0x5A                0xAA               0x0F               0x0F               0x0F    */
        (char)0b0101'1010, (char)0b1010'1010, (char)0b0000'1111, (char)0b0000'1111, (char)0b0000'1111
    };
    BitReader<false> bitReader( std::make_unique<BufferedFileReader>( fileContents ) );

    REQUIRE( bitReader.read<0>() == 0b0 );
    REQUIRE( bitReader.read<1>() == 0b0 );
    REQUIRE( bitReader.tell() == 1 );
    REQUIRE( bitReader.read<1>() == 0b1 );
    REQUIRE( bitReader.tell() == 2 );
    REQUIRE( bitReader.read<2>() == 0b10 );
    REQUIRE( bitReader.tell() == 4 );
    REQUIRE( bitReader.read<4>() == 0b0101 );
    REQUIRE( bitReader.tell() == 8 );
    REQUIRE( bitReader.read<8>() == 0b1010'1010 );
    REQUIRE( bitReader.tell() == 16 );
    REQUIRE( bitReader.read<8>() == 0b0000'1111 );
    REQUIRE( bitReader.tell() == 24 );

    REQUIRE( bitReader.seek( 0, SEEK_SET ) == 0 );
    REQUIRE( bitReader.tell() == 0 );
    REQUIRE( bitReader.read<8>() == 0b0101'1010 );
    REQUIRE( bitReader.tell() == 8 );
    /* Note that reading more than 8 bits, will result in the bytes being swapped!
     * This is because the byte numbering is from left to right but bit numbering from right to left,
     * but when we request more than 8 bits, all bits are numbered right to left in the resulting DWORD! */
    REQUIRE( bitReader.read<16>() == 0b0000'1111'1010'1010 );
    REQUIRE( bitReader.tell() == 24 );

    REQUIRE( bitReader.seek( -8, SEEK_CUR ) == 16 );
    REQUIRE( bitReader.tell() == 16 );
    REQUIRE( bitReader.read<8>() == 0b0000'1111 );
    REQUIRE( bitReader.tell() == 24 );

    REQUIRE( bitReader.seek( -8, SEEK_END ) == 32 );
    REQUIRE( bitReader.read<1>() == 0b1 );
    REQUIRE( bitReader.tell() == 33 );
    REQUIRE( bitReader.read<3>() == 0b111 );
    REQUIRE( bitReader.tell() == 36 );
    REQUIRE( bitReader.read<4>() == 0b0000 );
    REQUIRE( bitReader.tell() == 40 );
    REQUIRE( bitReader.eof() );

    REQUIRE( bitReader.seek( 0, SEEK_SET ) == 0 );
    REQUIRE( bitReader.read<32>() == 0x0F0F'AA5A );
    REQUIRE( bitReader.tell() == 32 );

    REQUIRE( bitReader.seek( 8, SEEK_SET ) == 8 );
    REQUIRE( bitReader.read<13>() == 0b00'1111'1010'1010 );
    REQUIRE( bitReader.tell() == 21 );

    REQUIRE( bitReader.seek( 0, SEEK_SET ) == 0 );
    REQUIRE( bitReader.read<4>() == 0xA );
    REQUIRE( bitReader.read<4>() == 0x5 );
    REQUIRE( bitReader.seek( 4, SEEK_SET ) == 4 );
    REQUIRE( bitReader.read<4>() == 0x5 );

    REQUIRE( bitReader.seek( 4, SEEK_SET ) == 4 );
    const auto result = bitReader.read<32>();
    REQUIRE( bitReader.tell() == 36 );
    REQUIRE( result == 0xF0F0'FAA5 );
    REQUIRE( bitReader.read<2>() == 0b00 );
    REQUIRE( bitReader.read<2>() == 0b00 );
    REQUIRE( bitReader.tell() == 40 );
    REQUIRE( bitReader.eof() );
}


int
main()
{
    REQUIRE( nLowestBitsSet<uint32_t>( 0 ) == uint32_t( 0b0000'0000U ) );
    REQUIRE( nLowestBitsSet<uint32_t>( 1 ) == uint32_t( 0b0000'0001U ) );
    REQUIRE( nLowestBitsSet<uint32_t>( 2 ) == uint32_t( 0b0000'0011U ) );
    REQUIRE( nLowestBitsSet<uint32_t>( 3 ) == uint32_t( 0b0000'0111U ) );
    REQUIRE( nLowestBitsSet<uint32_t>( 8 ) == uint32_t( 0b1111'1111U ) );
    REQUIRE( nLowestBitsSet<uint32_t>( 32 ) == uint32_t( 0xFFFF'FFFF ) );

    REQUIRE( nLowestBitsSet32<0 >() == uint32_t( 0b0000'0000U ) );
    REQUIRE( nLowestBitsSet32<1 >() == uint32_t( 0b0000'0001U ) );
    REQUIRE( nLowestBitsSet32<2 >() == uint32_t( 0b0000'0011U ) );
    REQUIRE( nLowestBitsSet32<3 >() == uint32_t( 0b0000'0111U ) );
    REQUIRE( nLowestBitsSet32<8 >() == uint32_t( 0b1111'1111U ) );
    REQUIRE( nLowestBitsSet32<32>() == uint32_t( 0xFFFF'FFFFU ) );

    REQUIRE( nLowestBitsSet<uint64_t>( 0 ) == uint64_t( 0b0000'0000U ) );
    REQUIRE( nLowestBitsSet<uint64_t>( 1 ) == uint64_t( 0b0000'0001U ) );
    REQUIRE( nLowestBitsSet<uint64_t>( 2 ) == uint64_t( 0b0000'0011U ) );
    REQUIRE( nLowestBitsSet<uint64_t>( 3 ) == uint64_t( 0b0000'0111U ) );
    REQUIRE( nLowestBitsSet<uint64_t>( 8 ) == uint64_t( 0b1111'1111U ) );
    REQUIRE( nLowestBitsSet<uint64_t>( 64 ) == uint64_t( 0xFFFF'FFFF'FFFF'FFFFULL ) );

    REQUIRE( nLowestBitsSet64<0 >() == uint64_t( 0b0000'0000U ) );
    REQUIRE( nLowestBitsSet64<1 >() == uint64_t( 0b0000'0001U ) );
    REQUIRE( nLowestBitsSet64<2 >() == uint64_t( 0b0000'0011U ) );
    REQUIRE( nLowestBitsSet64<3 >() == uint64_t( 0b0000'0111U ) );
    REQUIRE( nLowestBitsSet64<8 >() == uint64_t( 0b1111'1111U ) );
    REQUIRE( nLowestBitsSet64<64>() == uint64_t( 0xFFFF'FFFF'FFFF'FFFFULL ) );

    testMSBBitReader();
    testLSBBitReader();

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors;
}
