#pragma once

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#include <huffman/HuffmanCodingSymbolsPerLength.hpp>

#include "definitions.hpp"
#include "RFCTables.hpp"


namespace rapidgzip::deflate
{
/**
 * This started as a copy of @ref HuffmanCodingShortBitsCached, however, it incorporates deflate-specific things:
 *  - The idea is that the shortest codes are the most frequent ones and should be cached extensively, i.e.,
 *    even the following distance and lengths bits should be cached for these short codes.
 *  - In the most extreme case, codes would be 1-bit, but I doubt that there could be 10 (LUT_BITS_COUNT) of that
 *    because it would have to be 10 literals but at that point it should be stored as a length,distance=0-pair.
 *    So, 2-bits per code, i.e., caching 5 codes is probably the sane maximum and 3-4 should be the goal to cache.
 *  - In order to cache more than one code and not only literals and one non-literal, we would have to use the
 *    DistanceHuffman coding here to extract the distance during cache building.
 *  - In order to cache up to 4 results, we need to think somewhat about how to store everything. Thanks to
 *    limiting the LUT_BITS_COUNT from up to max. 16 to 10, we saved a lot of memory/cache but we still need to
 *    be careful.
 *    - We might save a branch somehow by always writing a symbol (even if it is a dummy one, which gets overwritten)
 *      and copying a back-reference (even if it is of length 0), or whether to do a conditional.
 *    - What we need:
 *      - Bits to skip: up to ceil(log2(LUT_BITS_COUNT)) = 4 bits
 *        -> this might be skipped by stopping at the first "invalid" cached symbol, e.g., when it is a back-reference
 *           with distance = 0, which is not allowed. This might also be good for loop unrolling.
 *      - Cached symbol count: 4 to 5 (and never should be 0?), so 3-4 bits.
 *      - Array of cached interpreted symbols:
 *        - Flag to tell whether the symbol is fully resolved or whether it still needs to be interpreted and
 *          length and distance need to be read or whether it needs to be tested against the EOB symbol.
 *          -> This probably would then toggle using a different union type.
 *          -> This could be encoded into the distance, i.e., if the distance is 0xFFFF, then it is a literal.
 *        - Symbol: 0-255 (never cache the EOB symbol here and distance/length gets interpreted) -> 8 bits
 *        - Length: 3-258 -> 8-bits or 9-bits if we want to avoid one add instruction although that would mean
 *                           that we need a | and & instruction to extract 9 bits as opposed to simply loading 1 B.
 *        - Distance: 1-32768, however the large distances would require reading 13 extra bits, which does not fit
 *                    the current optimal LUT_BITS_COUNT=10! For LUT_BITS_COUNT=10 and assuming at least 1 bit
 *                    for the length and 1 bit for the distance symbol, this would leave up to 8 bits for the distance
 *                    extra bits, yielding a maximum of 1024, i.e., 10 bits would be sufficient.
 *    - Cache entry sizes for each maximum cache count:
 *      - 3 entries: 4 + 2 + 3 * ( 1 + 8 + 8 + 10 ) bits = 87 bits
 *      - 4 entries: 4 + 3 + 4 * ( 1 + 8 + 8 + 10 ) bits = 115 bits (2x64 = 128)
 *      - 5 entries: 4 + 3 + 5 * ( 1 + 8 + 8 + 10 ) bits = 142 bits
 *    - Cache entry sizes for each maximum cache count when merging symbol and length in an union:
 *      - 3 entries: 4 + 2 + 3 * ( 1 + 8 + 10 ) bits = 63 bits -> would fit snuggly into 64 bits and leave space for
 *                                                                larger distances for larger LUT_BITS_COUNT.
 *      - 4 entries: 4 + 3 + 4 * ( 1 + 8 + 10 ) bits = 83 bits
 *      - 5 entries: 4 + 3 + 5 * ( 1 + 8 + 10 ) bits = 102 bits
 *    - Note that for HuffmanCodingShortBitsCached, the effective cache entry size is 32-bits out of which
 *      quite a lot are unused.
 *    - In order to reduce necessary bit-shifts for unpacking, it would be advisable to round some storages
 *      up to 8-bits.
 *  - [ ] Maybe start with caching multiple literals only.
 */
template<uint8_t  LUT_BITS_COUNT>
class HuffmanCodingShortBitsCachedDeflate :
    public HuffmanCodingSymbolsPerLength<uint16_t, MAX_CODE_LENGTH, uint16_t, MAX_LITERAL_HUFFMAN_CODE_COUNT,
                                         /* CHECK_OPTIMALITY */ true>
{
public:
    using Symbol = uint16_t;
    using HuffmanCode = uint16_t;
    using BaseType = HuffmanCodingSymbolsPerLength<HuffmanCode, MAX_CODE_LENGTH, Symbol, MAX_LITERAL_HUFFMAN_CODE_COUNT,
                                                   /* CHECK_OPTIMALITY */ true>;
    using BitCount = typename BaseType::BitCount;
    using CodeLengthFrequencies = typename BaseType::CodeLengthFrequencies;

    static constexpr size_t MAXIMUM_CACHED_SYMBOL_COUNT = 3U;

    struct CacheEntry
    {
        uint8_t bitsToSkip{ 0 };
        uint8_t symbolOrLength{ 0 };
        uint16_t distance{ 0 };
    };

    using BaseDistanceHuffmanCoding = HuffmanCodingBase<uint16_t, MAX_CODE_LENGTH, uint8_t, MAX_DISTANCE_SYMBOL_COUNT,
                                                        /* CHECK_OPTIMALITY */ true>;

public:
    [[nodiscard]] constexpr Error
    initializeFromLengths( const VectorView<BitCount>&  codeLengths,
                           const VectorView<BitCount>&  distanceCodeLengths )
    {
        if ( const auto errorCode = BaseType::initializeFromLengths( codeLengths );
             errorCode != Error::NONE )
        {
            return errorCode;
        }

        /* Compute min, max, and minimumCodeValuesPerLevel for the distance Huffman code so that it can be used
         * to fill the LUT. */
        BaseDistanceHuffmanCoding distanceHC;
        const auto distanceError = distanceHC.initializeFromLengths( distanceCodeLengths );
        if ( distanceError != Error::NONE ) {
            return distanceError;
        }

        m_bitsToReadAtOnce = std::max( LUT_BITS_COUNT, this->m_minCodeLength );

        /* Initialize the cache. */
        if ( m_needsToBeZeroed ) {
            // Works constexpr
            for ( size_t symbol = 0; symbol < m_codeCache.size(); ++symbol ) {
                m_codeCache[symbol].bitsToSkip = 0;
            }
        }

        if ( codeLengths.size() >= MAX_LITERAL_HUFFMAN_CODE_COUNT ) {
            return Error::INVALID_HUFFMAN_CODE;
        }

        auto codeValues = this->m_minimumCodeValuesPerLevel;
        for ( size_t symbol = 0; symbol < codeLengths.size(); ++symbol ) {
            const auto length = codeLengths[symbol];
            if ( ( length == 0 ) || ( length > LUT_BITS_COUNT ) ) {
                continue;
            }

            const auto reversedCode = reverseBits( codeValues[length - this->m_minCodeLength]++, length );
            /** @todo add exchaustive tests for all 11-bit combinations that could be cached. Maybe even test all 16-bit combinations and compare them to the existing Huffman Decoder + distance/length interpret code. */
            CacheEntry cacheEntry;
            cacheEntry.bitsToSkip = length;
            if ( symbol <= 255 ) {
                cacheEntry.symbolOrLength = static_cast<uint8_t>( symbol );
                cacheEntry.distance = 0;
                insertIntoCache( reversedCode, cacheEntry );
            } else if ( UNLIKELY( symbol == END_OF_BLOCK_SYMBOL /* 256 */ ) ) [[unlikely]] {
                cacheEntry.distance = 0xFFFFU;
                insertIntoCache( reversedCode, cacheEntry );
            } else if ( symbol <= 264U ) {
                cacheEntry.symbolOrLength = static_cast<uint8_t>( symbol - 257U );
                insertIntoCacheWithDistance( reversedCode, cacheEntry, distanceHC, distanceCodeLengths );
            } else if ( symbol < 285U ) {
                const auto lengthCode = static_cast<uint8_t>( symbol - 261U );
                const auto extraBitCount = lengthCode / 4;  /* <= 5 */
                /* Loop over all possible extra bits or skip filling if it does not fit into cache.
                 * We need left-over bits for the extra bits and at least one extra bit for the distance code,
                 * likely even more but that will be tested inside @ref insertIntoCacheWithDistance. */
                if ( length + extraBitCount + 1 <= LUT_BITS_COUNT ) {
                    cacheEntry.bitsToSkip += extraBitCount;
                    for ( uint8_t extraBits = 0; 1U << extraBitCount; ++extraBits ) {
                        cacheEntry.symbolOrLength = static_cast<uint8_t>(
                            calculateLength( lengthCode ) + reverseBits( extraBits, extraBitCount ) - 3U );
                        insertIntoCacheWithDistance( reversedCode | ( extraBits << length ), cacheEntry,
                                                     distanceHC, distanceCodeLengths );
                    }
                }
            } else if ( symbol == 285U ) {
                cacheEntry.symbolOrLength = 258U - 3U;
                insertIntoCacheWithDistance( reversedCode, cacheEntry, distanceHC, distanceCodeLengths );
            } else {
                assert( symbol < 286U /* MAX_LITERAL_HUFFMAN_CODE_COUNT */ );
            }
        }

        m_needsToBeZeroed = true;

        return Error::NONE;
    }

    template<typename BitReader,
             typename DistanceHuffmanCoding>
    [[nodiscard]] forceinline CacheEntry
    decode( BitReader&                   bitReader,
            const DistanceHuffmanCoding& distanceHC ) const
    {
        try {
            const auto& cacheEntry = m_codeCache[bitReader.peek( LUT_BITS_COUNT )];
            if ( cacheEntry.bitsToSkip == 0 ) {
                return decodeLong( bitReader, distanceHC );
            }
            bitReader.seekAfterPeek( cacheEntry.bitsToSkip );
            return cacheEntry;
        } catch ( const typename BitReader::EndOfFileReached& ) {
            /* Should only happen at the end of the file and probably not even there
             * because the bzip2 footer (EOS block) should be longer than the peek length. */
            return interpretSymbol( bitReader, distanceHC, BaseType::decode( bitReader ).value() );
        }
    }

private:
    template<typename BitReader,
             typename DistanceHuffmanCoding>
    [[nodiscard]] /* forceinline */ constexpr CacheEntry
    decodeLong( BitReader&                   bitReader,
                const DistanceHuffmanCoding& distanceHC ) const
    {
        HuffmanCode code = 0;

        /** Read the first n bytes. Note that we can't call the bitReader with argument > 1 because the bit order
         * would be inversed. @todo Reverse the Huffman codes and prepend bits instead of appending, so that this
         * first step can be conflated and still have the correct order for comparison! */
        for ( BitCount i = 0; i < m_bitsToReadAtOnce; ++i ) {
            code = ( code << 1U ) | ( bitReader.template read<1>() );
        }

        for ( BitCount k = m_bitsToReadAtOnce - this->m_minCodeLength;
              k <= this->m_maxCodeLength - this->m_minCodeLength; ++k )
        {
            const auto minCode = this->m_minimumCodeValuesPerLevel[k];
            if ( minCode <= code ) {
                const auto subIndex = this->m_offsets[k] + static_cast<size_t>( code - minCode );
                if ( subIndex < this->m_offsets[k + 1] ) {
                    return interpretSymbol( bitReader, distanceHC, this->m_symbolsPerLength[subIndex] );
                }
            }

            code <<= 1;
            code |= bitReader.template read<1>();
        }

        throw Error::INVALID_HUFFMAN_CODE;
    }

    template<typename BitReader,
             typename DistanceHuffmanCoding>
    [[nodiscard]] forceinline constexpr CacheEntry
    interpretSymbol( BitReader&                   bitReader,
                     const DistanceHuffmanCoding& distanceHC,
                     Symbol                       symbol ) const
    {
        CacheEntry cacheEntry{};

        if ( symbol <= 255 ) {
            cacheEntry.symbolOrLength = static_cast<uint8_t>( symbol );
            return cacheEntry;
        }

        if ( UNLIKELY( symbol == END_OF_BLOCK_SYMBOL /* 256 */ ) ) [[unlikely]] {
            cacheEntry.distance = 0xFFFFU;
            return cacheEntry;
        }

        if ( UNLIKELY( symbol > 285 ) ) [[unlikely]] {
            throw Error::INVALID_HUFFMAN_CODE;
        }

        cacheEntry.symbolOrLength = getLengthMinus3( symbol, bitReader );
        const auto [distance, error] = getDistance( CompressionType::DYNAMIC_HUFFMAN, distanceHC, bitReader );
        if ( error != Error::NONE ) {
            throw error;
        }
        cacheEntry.distance = distance;
        return cacheEntry;
    }

    forceinline void
    insertIntoCache( HuffmanCode reversedCode,
                     CacheEntry  cacheEntry )
    {
        if ( cacheEntry.bitsToSkip > LUT_BITS_COUNT ) {
            return;
        }

        const auto fillerBitCount = LUT_BITS_COUNT - cacheEntry.bitsToSkip;
        const auto maximumPaddedCode = static_cast<HuffmanCode>(
            reversedCode | ( nLowestBitsSet<HuffmanCode>( fillerBitCount ) << cacheEntry.bitsToSkip ) );
        assert( maximumPaddedCode < m_codeCache.size() );
        const auto increment = static_cast<HuffmanCode>( HuffmanCode( 1 ) << cacheEntry.bitsToSkip );
        for ( auto paddedCode = reversedCode; paddedCode <= maximumPaddedCode; paddedCode += increment ) {
            m_codeCache[paddedCode] = cacheEntry;
        }
    }

    forceinline void
    insertIntoCacheWithDistance( HuffmanCode                      reversedCode,
                                 const CacheEntry&                symbolAndLengthCacheEntry,
                                 const BaseDistanceHuffmanCoding& distanceHC,
                                 const VectorView<BitCount>       distanceCodeLengths )
    {
        return;
        if ( distanceCodeLengths.size() > 29U ) {
            throw std::logic_error( "Invalid distance codes encountered!" );
        }

        std::array<HuffmanCode, MAX_CODE_LENGTH + 1> codeValues{};
        for ( size_t symbol = 0; symbol < distanceCodeLengths.size(); ++symbol ) {
            const auto length = distanceCodeLengths[symbol];
            if ( ( length == 0 ) || ( length + symbolAndLengthCacheEntry.bitsToSkip > LUT_BITS_COUNT ) ) {
                continue;
            }

            const auto reversedDistanceCode = reverseBits( codeValues[length - distanceHC.minCodeLength()]++, length );
            const auto reversedCodeWithDistance =
                reversedCode | ( reversedDistanceCode << symbolAndLengthCacheEntry.bitsToSkip );

            auto cacheEntry = symbolAndLengthCacheEntry;
            cacheEntry.bitsToSkip += length;

            if ( symbol <= 3U ) {
                cacheEntry.distance = symbol + 1U;
                insertIntoCache( reversedCodeWithDistance, cacheEntry );
                continue;
            }

            assert( symbol <= 29U );
            const auto extraBitCount = ( symbol - 2U ) / 2U;
            if ( cacheEntry.bitsToSkip + extraBitCount >= LUT_BITS_COUNT ) {
                continue;
            }
            const auto extraBitsShift = cacheEntry.bitsToSkip;
            cacheEntry.bitsToSkip += extraBitCount;

            /* Loop over all possible extra bits. */
            for ( uint8_t extraBits = 0; 1U << extraBitCount; ++extraBits ) {
                cacheEntry.distance = distanceLUT[symbol] + extraBits;
                insertIntoCache( reversedCodeWithDistance | ( extraBits << extraBitsShift ), cacheEntry );
            }
        }
    }

private:
#if 0
    /**
     * Use struct of arrays to avoid large paddings.
     */
    struct CacheEntry
    {
        std::array<uint16_t, MAXIMUM_CACHED_SYMBOL_COUNT> distances{};
        std::array<uint8_t, MAXIMUM_CACHED_SYMBOL_COUNT> symbolsOrLengths{};
        uint8_t bitsToSkip{ 0 };
    };
    static_assert( sizeof( CacheEntry ) == 10 );
#endif
    alignas( 64 ) std::array<CacheEntry, ( 1UL << LUT_BITS_COUNT )> m_codeCache{};
    uint8_t m_bitsToReadAtOnce{ LUT_BITS_COUNT };
    bool m_needsToBeZeroed{ false };
};
}  // namespace rapidgzip::deflate
