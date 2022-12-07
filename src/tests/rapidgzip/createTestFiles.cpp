#include <array>
#include <fstream>
#include <stdexcept>
#include <vector>

#include <common.hpp>



/*
size=$(( 30*1024 ))
wordSize=24
head -c $size /dev/urandom > random-words
cat random-words > random-words-and-reversed
echo 'AAAAAAAAAAAAAAAAAAAAAAAA' >> random-words-and-reversed
echo 'BCBCBCBCBCBCBCBCBCBCBCBC' >> random-words-and-reversed
echo 'DEFDEFDEFDEFDEFDEFDEFDEF' >> random-words-and-reversed
echo 'GHIJGHIJGHIJGHIJGHIJGHIJ' >> random-words-and-reversed
echo 'KLMNOPKLMNOPKLMNOPKLMNOP' >> random-words-and-reversed
echo 'QRSTUVWXQRSTUVWXQRSTUVWX' >> random-words-and-reversed
echo 'abcdefghabcdefghabcdefgh' >> random-words-and-reversed
echo 'abcdefghijklabcdefghijkl' >> random-words-and-reversed
for (( i=0; i<size; i += wordSize )); do
    tail -c +$(( size - i - wordSize )) random-words | head -c "$wordSize" >> random-words-and-reversed
done
#hexdump -C random-words-and-reversed | head
#hexdump -C random-words-and-reversed | tail
igzip -0 -f -k random-words-and-reversed
src/tools/rapidgzip --analyze random-words-and-reversed.gz
*/

void
createMaxDistanceAlphabet()
{
    constexpr std::array<size_t, 21> DISTANCES = {
        16, 32, 64, 96,
        128, 192, 256, 384,
        512,  768, 1_Ki, 2_Ki,
        3_Ki, 4_Ki, 6_Ki, 8_Ki,
        12_Ki, 16_Ki, 24_Ki, 32_Ki,
        /* dummy */ 34_Ki
    };

    std::vector<char> data( 35_Ki );
    for ( auto& x : data ) {
        x = static_cast<char>( rand() % 256 );
    }

    /**
     * @verbatim
     * CCCCBBA..ABBCCCC
     * @endverbatim
     * To create a duplicate of length l at distance d, we need at least d + l memory.
     * To avoid self-collision: l <= d.
     * To also leave at least s = d - l space inside the skipped distance: l <= d - s.
     * With inner length l, distance d and outer outer length L, distance D, we get:
     * d + 2 * l = s and L <= D - s <=> L <= D - d - l.
     *
     * This yields a recursive formula:
     *  l0 for d0 = 32: Set to 32 -> s = 64
     *  l1 for d1 = 64: 64 - 32 - 32 = 0  -> Does not work, so halve l0
     * Better:
     *  l0 for d0 = 32: Set to 16 -> s = 48
     *  l1 for d1 = 64: 64 - 32 - 16 = 16  -> Does not work, so halve l0
     * The total required space then sums up to:
     *  s_{i+1} = s_i + 2 * l_i
     *
     * We want the next duplicate to fit into that space, which gives us a formula for the l to use.
     * It might be easier to construct it from inside out.
     * The innermost is d = 32 taking up d + l <= 64 giving l <= 32.
     * For the next one d = 64 taking up d + l <= 96 giving l <= 32.
     */
    long int offset{ static_cast<long int>( data.size() ) / 2 };
    long int length{ 8 };
    for ( size_t i = 0; i + 1 < DISTANCES.size(); ++i ) {
        const auto distance = DISTANCES[i];
        const auto nextDistance = DISTANCES[i + 1];

        const auto duplicateOffset = offset + distance;
        std::copy( data.begin() + offset, data.begin() + offset + length, data.begin() + duplicateOffset );

        if ( i + 1 == DISTANCES.size() ) {
            break;
        }

        length = nextDistance - distance - length;
        std::cerr << "offset: " << offset << ", i: " << i << ", length: " << length
                  << ", distance: " << distance << ", next distance: " << nextDistance << "\n";
        if ( length > offset ) {
            throw std::logic_error( "Out of bounds!" );
        }
        offset -= length;
    }

    std::ofstream file( "duplicates-at-varying-distances" );
    file.write( data.data(), static_cast<std::streamsize>( data.size() ) );
}


int
main()
{
    createMaxDistanceAlphabet();
    return 0;
}


/*
cmake --build . -- createTestFiles && src/tests/rapidgzip/createTestFiles
igzip -0 -f -k duplicates-at-varying-distances
src/tools/rapidgzip --analyze duplicates-at-varying-distances.gz | grep -A 3 Alphabets

    Huffman Alphabets:
        Precode  : 14 CLs in [2, 7] out of 19: CL:Count, 0:5, 2:2, 3:1, 4:5, 6:2, 7:4,
        Distance : 30 CLs in [3, 9] out of 30: CL:Count, 3:1, 4:7, 5:11, 6:4, 7:2, 8:3, 9:2,
        Literals : 286 CLs in [2, 15] out of 286: CL:Count, 2:1, 4:2, 5:3, 6:8, 7:13, 8:21, 9:54, 10:63, 11:112, 12:2, 13:3, 15:4,
*/
