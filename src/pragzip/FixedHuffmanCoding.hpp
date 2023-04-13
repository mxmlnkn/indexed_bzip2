#pragma once

#include <cstdint>

#include <HuffmanCodingReversedBitsCached.hpp>

#include "definitions.hpp"


namespace pragzip::deflate
{
/**
 * Because the fixed Huffman coding is used by different threads it HAS TO BE immutable. It is constant anyway
 * but it also MUST NOT have mutable members. This means that HuffmanCodingDoubleLiteralCached does NOT work
 * because it internally saves the second symbol.
 * @todo Make it such that the implementations can handle the case that the construction might result in
 *       larger symbol values than are allowed to appear in the first place! I.e., cut-off construction there.
 *       Note that changing this from 286 to 512, lead to an increase of the runtime! We need to reduce it again! */
using FixedHuffmanCoding =
    HuffmanCodingReversedBitsCached<uint16_t, MAX_CODE_LENGTH, uint16_t, MAX_LITERAL_OR_LENGTH_SYMBOLS + 2>;


/* Initializing m_fixedHC statically leads to very weird problems when compiling with ASAN.
 * The code might be too complex and run into the static initialization order fiasco.
 * But having this static const is very important to get a 10-100x speedup for finding deflate blocks! */
extern const FixedHuffmanCoding FIXED_HC;
}  // namespace pragzip::deflate
