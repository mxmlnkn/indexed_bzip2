#pragma once

#include <limits>
#include <utility>

#include <BitReader.hpp>
#include <common.hpp>
#include <definitions.hpp>


namespace rapidgzip::blockfinder
{
/**
 * This function searches for gzip stream headers (0x1F 8B 08), which have basically 256x less false positives
 * than looking for uncompressed blocks. It can also check the 3 reserved flags that are to be 0.
 *
 * @todo Measure and improve performance by using std::string_view::find in the BitReader byte buffer.
 *       This would require some additional BitReader interfaces for accessing and forwarding the
 *       internal buffer: getBytesBufferView, realignBuffer / reloadBuffer something like that.
 *       Many problems: what if at the current position the data for some bytes only exists in the bit buffer?
 *       hard to return a correct char* view to that. Fall back to the slower read method in this case?
 * @todo Needs tests not just the benchmarks
 *
 * @return An offset containing a possible gzip stream header.
 *         Returns std::numeric_limits<size_t>::max if nothing was found.
 */
[[nodiscard]] inline size_t
seekToGzipStreamHeader( BitReader&   bitReader,
                        size_t const untilOffset = std::numeric_limits<size_t>::max() )
{
    try
    {
        const auto startOffset = bitReader.tell();
        /* Align to byte because we begin checking there instead of the deflate magic bits. */
        const auto startOffsetByte = ceilDiv( startOffset, BYTE_SIZE ) * BYTE_SIZE;
        if ( startOffsetByte < untilOffset ) {
            bitReader.seek( static_cast<long long int>( startOffsetByte ) );
        }

        uint32_t magicBytes{ 0 };
        for ( size_t i = 0; i < 2; ++i ) {
            magicBytes = ( magicBytes >> BYTE_SIZE ) | ( bitReader.read<BYTE_SIZE>() << 2U * BYTE_SIZE );
        }
        for ( size_t offset = startOffsetByte; offset < untilOffset; offset += BYTE_SIZE ) {
            magicBytes = ( magicBytes >> BYTE_SIZE ) | ( bitReader.read<BYTE_SIZE>() << 2U * BYTE_SIZE );
            if ( LIKELY( magicBytes != gzip::MAGIC_BYTES_GZIP ) ) [[likely]] {
                continue;
            }

            const auto flags = bitReader.peek<BYTE_SIZE>();
            if ( LIKELY( ( flags & 0b1110'0000ULL ) != 0ULL ) ) [[likely]] {
                continue;
            }

            return offset;
        }
    } catch ( const BitReader::EndOfFileReached& ) {
        /* This might happen when trying to read the 32 bits! */
    }

    return std::numeric_limits<size_t>::max();
}
}  // namespace rapidgzip::blockfinder
