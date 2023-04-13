#pragma once

#include <cstdint>
#include <limits>

#include <BitReader.hpp>


namespace pragzip::blockfinder
{
using BitReader = ::BitReader<false, uint64_t>;


/**
 * @see benchmarkLUTSize
 * This highly depends on the implementation of the for loop over the bitReader.
 * - Earliest versions without checkPrecode showed best results for 18 bits for this LUT.
 * - Versions with checkPrecode showed best results for 16 bits.
 * - The version that keeps two bit buffers to avoid back-seeks was optimal with 13 bits probably
 *   because that saves an additional shift when moving bits from one bit buffer to the other while avoiding
 *   duplicated bits because there is no duplication for 13 bits.
 * - The version with manual bit buffers and HuffmanCodingReversedCodesPerLength for the precode
 *   is fastest with 15 bits.
 * - The version with manual bit buffers and no call to Block::readDynamicHuffmanCoding, which uses
 *   HuffmanCodingCheckOnly is fastest with 14 bits.
 */
static constexpr uint8_t OPTIMAL_NEXT_DEFLATE_LUT_SIZE = 14;


/**
 * Same as findDeflateBlocksPragzip but prefilters calling pragzip using a lookup table and even skips multiple bits.
 * Also, does not find uncompressed blocks nor fixed huffman blocks and as the others no final blocks!
 * The idea is that fixed huffman blocks should be very rare and uncompressed blocks can be found very fast in a
 * separate run over the data (to be implemented).
 *
 * @param untilOffset All returned matches will be smaller than this offset or `std::numeric_limits<size_t>::max()`.
 */
template<uint8_t CACHED_BIT_COUNT = OPTIMAL_NEXT_DEFLATE_LUT_SIZE>
[[nodiscard]] size_t
seekToNonFinalDynamicDeflateBlock( BitReader&   bitReader,
                                   size_t const untilOffset = std::numeric_limits<size_t>::max() );
}  // pragzip::blockfinder


#ifdef WITH_PYTHON_SUPPORT
    #include <DynamicHuffman.tpp>
#endif
