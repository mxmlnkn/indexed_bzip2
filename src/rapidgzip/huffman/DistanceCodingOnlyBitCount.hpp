#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

#include "HuffmanCodingSymbolsPerLength.hpp"


namespace rapidgzip::deflate
{
/**
 * This version uses a large lookup table (LUT) to avoid loops over the BitReader to speed things up a lot.
 * The problem is that the LUT creation can take a while depending on the code lengths.
 * - During initialization, it creates a LUT. The index of that array are a fixed number of bits read from BitReader.
 *   To simplify things, the fixed bits must be larger or equal than the maximum code length.
 *   To fill the LUT, the higher bits the actual codes with shorter lengths are filled with all possible values
 *   and the LUT table result is duplicated for all those values. This process is slow.
 * - During decoding, it reads MAX_CODE_LENGTH bits from the BitReader and uses that value to access the LUT,
 *   which contains the symbol and the actual code length, which is <= MAX_CODE_LENGTH. The BitReader will be seeked
 *   by the actual code length.
 */
template<typename HuffmanCode,
         uint8_t  MAX_CODE_LENGTH,
         typename Symbol,
         size_t   MAX_SYMBOL_COUNT>
class DistanceCodingOnlyBitCount :
    public HuffmanCodingSymbolsPerLength<HuffmanCode, MAX_CODE_LENGTH, Symbol, MAX_SYMBOL_COUNT>
{
public:
    using BaseType = HuffmanCodingSymbolsPerLength<HuffmanCode, MAX_CODE_LENGTH, Symbol, MAX_SYMBOL_COUNT>;
    using BitCount = typename BaseType::BitCount;
    using CodeLengthFrequencies = typename BaseType::CodeLengthFrequencies;

public:
    [[nodiscard]] constexpr Error
    initializeFromLengths( const VectorView<BitCount>& codeLengths )
    {
        if ( const auto errorCode = BaseType::initializeFromLengths( codeLengths );
             errorCode != Error::NONE )
        {
            return errorCode;
        }

        if ( m_needsToBeZeroed ) {
            for ( size_t symbol = 0; symbol < ( 1ULL << this->m_maxCodeLength ); ++symbol ) {
                m_codeCache[symbol].first = 0;
            }
        }

        auto codeValues = this->m_minimumCodeValuesPerLevel;
        for ( size_t symbol = 0; symbol < codeLengths.size(); ++symbol ) {
            const auto length = codeLengths[symbol];
            if ( length == 0 ) {
                continue;
            }

            const auto k = length - this->m_minCodeLength;
            const auto code = codeValues[k]++;
            const auto reversedCode = reverseBits( code, length );

            const auto fillerBitCount = this->m_maxCodeLength - length;
            const auto maximumPaddedCode = static_cast<HuffmanCode>(
                reversedCode | ( nLowestBitsSet<HuffmanCode>( fillerBitCount ) << length ) );
            assert( maximumPaddedCode < m_codeCache.size() );
            const auto increment = static_cast<HuffmanCode>( HuffmanCode( 1 ) << length );
            for ( auto paddedCode = reversedCode; paddedCode <= maximumPaddedCode; paddedCode += increment ) {
                const auto extraBitsCount = symbol <= 3U ? 0U : ( symbol - 2U ) / 2U;
                m_codeCache[paddedCode].first = static_cast<uint8_t>( length + extraBitsCount );
                m_codeCache[paddedCode].second = static_cast<Symbol>( symbol );  // @todo
            }
        }

        m_needsToBeZeroed = true;

        return Error::NONE;
    }

    [[nodiscard]] forceinline std::optional<Symbol>
    decode( BitReader& bitReader ) const
    {
        try {
            const auto value = bitReader.peek( this->m_maxCodeLength );

            assert( value < m_codeCache.size() );
            const auto [length, symbol] = m_codeCache[(int)value];

            if ( length == 0 ) {
                /* This might happen for non-optimal Huffman trees out of which all except the case of a single
                 * symbol with bit length 1 are forbidden! */
                return std::nullopt;
            }

            bitReader.read( length );
            return symbol;
        } catch ( const BitReader::EndOfFileReached& ) {
            /* Should only happen at the end of the file and probably not even there
             * because the gzip footer should be longer than the peek length. */
            return BaseType::decode( bitReader );
        }
    }

private:
    alignas( 8 ) std::array</* length + extra bits */ std::pair<uint8_t, Symbol>, ( 1UL << MAX_CODE_LENGTH )>
    m_codeCache{};
    bool m_needsToBeZeroed{ false };
};
}  // namespace rapidgzip
