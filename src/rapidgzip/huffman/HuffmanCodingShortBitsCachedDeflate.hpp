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
 *  - Default arguments for HuffmanCode and Symbol types as well as assuming REVERSE_BITT=true.
 */
template<uint8_t LUT_BITS_COUNT>
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
    using BaseDistanceHuffmanCoding = HuffmanCodingBase<uint16_t, MAX_CODE_LENGTH, uint8_t, MAX_DISTANCE_SYMBOL_COUNT,
                                                        /* CHECK_OPTIMALITY */ true>;

    struct CacheEntry
    {
        uint8_t bitsToSkip{ 0 };
        uint8_t symbolOrLength{ 0 };
        uint16_t distance{ 0 };
    };

    struct DistanceCode
    {
        uint8_t bitsToSkip{ 0 };
        uint8_t reversedCode{ 0 };
        uint8_t symbol{ 0 };
    };

public:
    [[nodiscard]] constexpr Error
    initializeFromLengths( const VectorView<BitCount>& codeLengths,
                           const VectorView<BitCount>& distanceCodeLengths )
    {
        if ( const auto errorCode = BaseType::initializeFromLengths( codeLengths );
             errorCode != Error::NONE )
        {
            return errorCode;
        }

        if ( const auto errorCode = initializeDistanceCodesFromLengths( distanceCodeLengths );
             errorCode != Error::NONE )
        {
            return errorCode;
        }

        m_lutBitsCount = std::min( LUT_BITS_COUNT, this->m_maxCodeLength );
        m_bitsToReadAtOnce = std::max( LUT_BITS_COUNT, this->m_minCodeLength );

        /* Initialize the cache. */
        if ( m_needsToBeZeroed ) {
            // Works constexpr
            for ( size_t symbol = 0; symbol < m_codeCache.size(); ++symbol ) {
                m_codeCache[symbol].bitsToSkip = 0;
            }
        }

        auto codeValues = this->m_minimumCodeValuesPerLevel;
        for ( size_t symbol = 0; symbol < codeLengths.size(); ++symbol ) {
            const auto length = codeLengths[symbol];
            if ( ( length == 0 ) || ( length > m_lutBitsCount ) ) {
                continue;
            }

            const auto reversedCode = reverseBits( codeValues[length - this->m_minCodeLength]++, length );
            CacheEntry cacheEntry{};
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
                insertIntoCacheWithDistance( reversedCode, cacheEntry );
            } else if ( symbol < 285U ) {
                const auto lengthCode = static_cast<uint8_t>( symbol - 261U );
                const auto extraBitCount = lengthCode / 4;  /* <= 5 */
                /* Loop over all possible extra bits or skip filling if it does not fit into cache.
                 * We need left-over bits for the extra bits and at least one extra bit for the distance code,
                 * likely even more but that will be tested inside @ref insertIntoCacheWithDistance. */
                if ( length + extraBitCount + 1 <= m_lutBitsCount ) {
                    continue;
                    cacheEntry.bitsToSkip += extraBitCount;
                    for ( uint8_t extraBits = 0; extraBits < ( 1U << extraBitCount ); ++extraBits ) {
                        cacheEntry.symbolOrLength = static_cast<uint8_t>(
                            calculateLength( lengthCode ) + reverseBits( extraBits, extraBitCount ) - 3U );
                        insertIntoCacheWithDistance( reversedCode | ( extraBits << length ), cacheEntry );
                    }
                }
            } else if ( symbol == 285U ) {
                cacheEntry.symbolOrLength = 258U - 3U;
                insertIntoCacheWithDistance( reversedCode, cacheEntry );
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
            const auto cacheEntry = m_codeCache[bitReader.peek( m_lutBitsCount )];
            if ( cacheEntry.bitsToSkip == 0 ) {
                return decodeLong( bitReader, distanceHC );
            }
            bitReader.seekAfterPeek( cacheEntry.bitsToSkip );
            if ( cacheEntry.distance == 0xFFFEU ) {
                return interpretSymbol( bitReader, distanceHC, cacheEntry.symbolOrLength + 257U );
            }
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
    [[nodiscard]] forceinline constexpr CacheEntry
    decodeLong( BitReader&                   bitReader,
                const DistanceHuffmanCoding& distanceHC ) const
    {
        HuffmanCode code = 0;

        for ( BitCount i = 0; i < this->m_minCodeLength; ++i ) {
            code = ( code << 1U ) | ( bitReader.template read<1>() );
        }

        for ( BitCount k = 0; k <= this->m_maxCodeLength - this->m_minCodeLength; ++k ) {
            const auto minCode = this->m_minimumCodeValuesPerLevel[k];
            if ( minCode <= code ) {
                const auto subIndex = m_offsets[k] + static_cast<size_t>( code - minCode );
                if ( subIndex < m_offsets[k + 1] ) {
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
    insertIntoCache( const HuffmanCode reversedCode,
                     const CacheEntry  cacheEntry )
    {
        const auto length = cacheEntry.bitsToSkip;
        if ( length > m_lutBitsCount ) {
            return;
        }
        const auto fillerBitCount = m_lutBitsCount - length;

        const auto maximumPaddedCode = static_cast<HuffmanCode>(
            reversedCode | ( nLowestBitsSet<HuffmanCode>( fillerBitCount ) << length ) );
        assert( maximumPaddedCode < m_codeCache.size() );
        const auto increment = static_cast<HuffmanCode>( HuffmanCode( 1 ) << length );
        for ( auto paddedCode = reversedCode; paddedCode <= maximumPaddedCode; paddedCode += increment ) {
            m_codeCache[paddedCode] = cacheEntry;
        }
    }

    forceinline void
    insertIntoCacheWithDistance( HuffmanCode       reversedCode,
                                 const CacheEntry& symbolAndLengthCacheEntry )
    {
        for ( size_t i = 0; i < m_distanceCodesCount; ++i ) {
            const auto distanceCode = m_distanceCodes[i];
            const auto length = distanceCode.bitsToSkip;
            if ( symbolAndLengthCacheEntry.bitsToSkip + length > m_lutBitsCount ) {
                continue;
            }

            const auto symbol = distanceCode.symbol;
            const auto reversedCodeWithDistance =
                reversedCode | ( distanceCode.reversedCode << symbolAndLengthCacheEntry.bitsToSkip );

            auto cacheEntry = symbolAndLengthCacheEntry;
            cacheEntry.bitsToSkip += length;
            assert( ( reversedCodeWithDistance & nLowestBitsSet<HuffmanCode>( cacheEntry.bitsToSkip ) )
                    == reversedCodeWithDistance );

            if ( symbol <= 3U ) {
                cacheEntry.distance = symbol + 1U;
                //insertIntoCache( reversedCodeWithDistance, cacheEntry );
                continue;
            }

            assert( symbol <= 29U );
            const auto extraBitCount = ( symbol - 2U ) / 2U;
            if ( extraBitCount > 0 ) {
                continue;
            }
            if ( cacheEntry.bitsToSkip + extraBitCount >= m_lutBitsCount ) {
                continue;
            }
            const auto extraBitsShift = cacheEntry.bitsToSkip;
            cacheEntry.bitsToSkip += extraBitCount;

            /* Loop over all possible extra bits. */
            for ( uint8_t extraBits = 0; extraBits < ( 1U << extraBitCount ); ++extraBits ) {
                cacheEntry.distance = distanceLUT[symbol] + reverseBits( extraBits, extraBitCount );
                insertIntoCache( reversedCodeWithDistance | ( extraBits << extraBitsShift ), cacheEntry );
            }
        }
    }

    [[nodiscard]] Error
    initializeDistanceCodesFromLengths( const VectorView<BitCount> codeLengths )
    {
        /* Compute min, max, and minimumCodeValuesPerLevel for the distance Huffman code so that it can be used
         * to fill the LUT. */
        BaseDistanceHuffmanCoding distanceHC;
        if ( const auto error = distanceHC.initializeFromLengths( codeLengths ); error != Error::NONE ) {
            return error;
        }

#if 1
        HuffmanCodingReversedBitsCached<uint16_t, MAX_CODE_LENGTH, uint8_t, MAX_DISTANCE_SYMBOL_COUNT>
        pristineDistanceHC;
        if ( const auto error = pristineDistanceHC.initializeFromLengths( codeLengths ); error != Error::NONE ) {
            return error;
        }
        const auto& distanceCodeCache = pristineDistanceHC.codeCache();
#endif
        m_distanceCodesCount = 0;
        auto codeValues = distanceHC.minimumCodeValuesPerLevel();

        for ( size_t symbol = 0; symbol < codeLengths.size(); ++symbol ) {
            const auto length = codeLengths[symbol];
            if ( length == 0 ) {
                continue;
            }

            const auto code = codeValues[length - distanceHC.minCodeLength()]++;
#if 1
            const auto reversedCode = reverseBits( code, length );

            const auto& [length2, symbol2] = distanceCodeCache.at( reversedCode );
            if ( length != length2 ) {
                std::cerr << "length: " << (int)length << ", length2: " << (int)length2 << "\n";
                throw 2;
            }
            if ( symbol != symbol2 ) {
                std::cerr << "symbol: " << (int)symbol << ", symbol2: " << (int)symbol2 << "\n";
                throw 3;
            }
#endif
            auto& distanceCode = m_distanceCodes[m_distanceCodesCount++];
            distanceCode.bitsToSkip = length;
            distanceCode.reversedCode = reverseBits( code, length );
            distanceCode.symbol = symbol;
        }

        return Error::NONE;
    }

private:
    alignas( 64 ) std::array<CacheEntry, ( 1UL << LUT_BITS_COUNT )> m_codeCache{};
    std::array<DistanceCode, MAX_DISTANCE_SYMBOL_COUNT> m_distanceCodes{};
    uint8_t m_distanceCodesCount{ 0 };

    uint8_t m_lutBitsCount{ LUT_BITS_COUNT };
    uint8_t m_bitsToReadAtOnce{ LUT_BITS_COUNT };
    bool m_needsToBeZeroed{ false };
};
}  // namespace rapidgzip::deflate
