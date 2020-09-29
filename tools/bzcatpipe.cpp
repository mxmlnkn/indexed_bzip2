#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <bzip2.h>
#include <BitReader.hpp>


int main( int argc, char** argv )
{
    BZ2Reader reader( STDIN_FILENO );
    size_t nBytesWrittenTotal = 0;
    do {
        std::vector<char> buffer( 10*1024*1024, 0 );
        const size_t nBytesRead = reader.read( -1, buffer.data(), buffer.size() );
        assert( nBytesRead <= buffer.size() );
        write( STDOUT_FILENO, buffer.data(), nBytesRead );
        nBytesWrittenTotal += nBytesRead;
        if ( nBytesRead < buffer.size() ) {
            break;
        }
    } while ( !reader.eof() );

    const auto offsets = reader.blockOffsets();
    std::cerr << "Encoded stream size : " << reader.tellCompressed() / 8 << " B\n";
    std::cerr << "Decoded stream size : " << nBytesWrittenTotal << " B\n";
    std::cerr << "Calculated CRC      : 0x" << std::hex << reader.crc() << std::dec << "\n";

    std::ofstream outfile( "bz2blockoffset.json", std::ios::out );
    outfile << "{\n";
    for ( auto it = offsets.begin(); it != offsets.end(); ++it ) {
        if ( it != offsets.begin() ) {
            outfile << ",\n";
        }
        outfile << "  " << it->first << ":" << it->second;
    }
    outfile << "\n}\n";

    return 0;
}
