#include "FixedHuffmanCoding.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>

#include "definitions.hpp"
#include "Error.hpp"


namespace pragzip::deflate
{
[[nodiscard]] constexpr FixedHuffmanCoding
createFixedHC()
{
    std::array<uint8_t, MAX_LITERAL_OR_LENGTH_SYMBOLS + 2> encodedFixedHuffmanTree{};
    for ( size_t i = 0; i < encodedFixedHuffmanTree.size(); ++i ) {
        if ( i < 144 ) {
            encodedFixedHuffmanTree[i] = 8;
        } else if ( i < 256 ) {
            encodedFixedHuffmanTree[i] = 9;
        } else if ( i < 280 ) {
            encodedFixedHuffmanTree[i] = 7;
        } else {
            encodedFixedHuffmanTree[i] = 8;
        }
    }

    FixedHuffmanCoding result;
    const auto error = result.initializeFromLengths( { encodedFixedHuffmanTree.data(),
                                                       encodedFixedHuffmanTree.size() } );
    if ( error != Error::NONE ) {
        throw std::logic_error( "Fixed Huffman Tree could not be created!" );
    }

    return result;
}


constexpr auto FIXED_HC_COMPILE_TIME = createFixedHC();


/* Initializing m_fixedHC statically leads to very weird problems when compiling with ASAN.
 * The code might be too complex and run into the static initialization order fiasco.
 * But having this static const is very important to get a 10-100x speedup for finding deflate blocks! */
const FixedHuffmanCoding FIXED_HC = FIXED_HC_COMPILE_TIME;
}  // namespace pragzip::deflate
