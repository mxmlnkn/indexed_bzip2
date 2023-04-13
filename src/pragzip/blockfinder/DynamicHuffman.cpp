#include "DynamicHuffman.tpp"


namespace pragzip::blockfinder
{
template size_t
seekToNonFinalDynamicDeflateBlock<OPTIMAL_NEXT_DEFLATE_LUT_SIZE>( BitReader&   bitReader,
                                                                  size_t const untilOffset );
}  // pragzip::blockfinder
