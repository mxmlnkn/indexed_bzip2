#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

#include <RFCTables.hpp>

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
 * Based on @ref HuffmanCodingReversedBitsCached.
 */
template<typename HuffmanCode,
         uint8_t  MAX_CODE_LENGTH,
         typename Symbol,
         size_t   MAX_SYMBOL_COUNT>
class DistanceCoding :
    public HuffmanCodingSymbolsPerLength<HuffmanCode, MAX_CODE_LENGTH, Symbol, MAX_SYMBOL_COUNT>
{
public:
    using BaseType = HuffmanCodingSymbolsPerLength<HuffmanCode, MAX_CODE_LENGTH, Symbol, MAX_SYMBOL_COUNT>;
    using BitCount = typename BaseType::BitCount;
    using CodeLengthFrequencies = typename BaseType::CodeLengthFrequencies;

    /* Values higher than 14 begin to make not much sense because the maximum extra bits are 13 and the minimum
     * code length is 1. Larger values will result in more time being spent in LUT creation and also will increase
     * cache pressure. */
    static constexpr size_t MINIMUM_LUT_BITS = 8U;
    static_assert( MINIMUM_LUT_BITS <= MAX_CODE_LENGTH, "The LUT only is sized for MAX_CODE_LENGTH!" );

public:
    [[nodiscard]] constexpr Error
    initializeFromLengths( const VectorView<BitCount>& codeLengths )
    {
        if ( const auto errorCode = BaseType::initializeFromLengths( codeLengths );
             errorCode != Error::NONE )
        {
            return errorCode;
        }

        m_LUTBits = std::max<uint8_t>( this->m_maxCodeLength, MINIMUM_LUT_BITS );

        if ( m_needsToBeZeroed ) {
            for ( size_t symbol = 0; symbol < ( 1ULL << m_LUTBits ); ++symbol ) {
                m_codeCache[symbol] = Entry{};
            }
        }

        auto codeValues = this->m_minimumCodeValuesPerLevel;
        for ( size_t symbol = 0; symbol < codeLengths.size(); ++symbol ) {
            const auto length = codeLengths[symbol];
            if ( length == 0 ) {
                continue;
            }

            const auto extraBitsCount = calculateDistanceExtraBits( static_cast<uint16_t>( symbol ) );
            const auto reversedCode = reverseBits( codeValues[length - this->m_minCodeLength]++, length );
            const auto totalLength = static_cast<uint8_t>( length + extraBitsCount );
            const auto canBeDoubleCached = totalLength <= m_LUTBits;

            Entry entry{};
            entry.extraBitsCount = canBeDoubleCached ? 0 : extraBitsCount;

            if ( !canBeDoubleCached && ( extraBitsCount == 0 ) ) {
                std::cerr << "length: " << (int)length << ", extraBitsCount: " << (int)extraBitsCount
                          << ", m_maxCodeLength: " << (int)this->m_maxCodeLength << ", m_LUTBits: " << (int)m_LUTBits << "\n";
                throw 3;
            }

            const auto fillerBitCount = m_LUTBits - length;
            const auto maximumPaddedCode = static_cast<HuffmanCode>(
                reversedCode | ( nLowestBitsSet<HuffmanCode>( fillerBitCount ) << length ) );
            assert( maximumPaddedCode < m_codeCache.size() );
            const auto increment = static_cast<HuffmanCode>( HuffmanCode( 1 ) << length );
            for ( auto paddedCode = reversedCode; paddedCode <= maximumPaddedCode; paddedCode += increment ) {
                entry.bitCount = canBeDoubleCached ? totalLength : length;
                entry.distanceOrCode = canBeDoubleCached
                                       ? getDistance( static_cast<uint16_t>( symbol ), extraBitsCount,
                                                      paddedCode >> length )
                                       : symbol;
                m_codeCache[paddedCode] = entry;
            }
        }

        m_needsToBeZeroed = true;

        return Error::NONE;
    }

    [[nodiscard]] forceinline std::optional<uint16_t>
    decode( BitReader& bitReader ) const
    {
        try {
            const auto value = bitReader.peek( m_LUTBits );

            assert( value < m_codeCache.size() );
            const auto& entry = m_codeCache[(int)value];
            bitReader.seekAfterPeek( entry.bitCount );

            //std::cerr << "peek: " << m_LUTBits << ", seekAfterPeek: " << (int)entry.bitCount << "\n";

            if UNLIKELY( ( entry.bitCount == 0 ) ) [[unlikely]] {
                /* This might happen for non-optimal Huffman trees out of which all except the case of a single
                 * symbol with bit length 1 are forbidden! */
                std::cerr << "return nullopt!\n";
                return std::nullopt;
            }

            if LIKELY( ( entry.extraBitsCount == 0 ) ) [[likely]] {
                //std::cerr << "Return distance: " << entry.distanceOrCode << " with bit count: " << (int) entry.bitCount << "\n";
                return entry.distanceOrCode;
            }

            const auto extraBits = bitReader.read( entry.extraBitsCount );
            const auto distance = getDistance( entry.distanceOrCode, entry.extraBitsCount, extraBits );
            //if ( distance >= 32768 ) {
            //std::cerr << "distance: " << distance << ", entry.distanceOrCode: " << entry.distanceOrCode
            //          << ", entry.extraBitsCount: " << (int)entry.extraBitsCount << "\n";
            //    throw 3;
            //}
            return distance;
        } catch ( const BitReader::EndOfFileReached& ) {
            /* Should only happen at the end of the file and probably not even there
             * because the gzip footer should be longer than the peek length. */
            std::cerr << "EOF, decode with base type\n";
            const auto distanceCode = BaseType::decode( bitReader );
            if ( !distanceCode ) {
                return std::nullopt;
            }
            const auto extraBitsCount = calculateDistanceExtraBits( *distanceCode );
            return getDistance( *distanceCode, extraBitsCount, bitReader.read( extraBitsCount ) );
        }
    }

private:
    [[nodiscard]] static constexpr uint16_t
    getDistance( const uint16_t distanceCode,
                 const uint8_t  extraBitsCount,
                 const uint16_t nextBits )
    {
        if ( distanceCode <= 3U ) {
            return distanceCode + 1U;
        }
        if ( distanceCode <= 29U ) {
            const auto extraBits = nextBits & nLowestBitsSet<uint16_t>( extraBitsCount );
            return distanceLUT[distanceCode] + extraBits;
        }
        throw std::logic_error( "Invalid distance code encountered during LUT creation!" );
    }

private:
    struct Entry
    {
        uint16_t distanceOrCode{ 0 };
        uint8_t  bitCount{ 0 };  // code length (<= 15) + extra bits (<= 13)
        uint8_t  extraBitsCount{ 0 };  // Only non-zero if extra bits have not been read yet
    };

    uint16_t m_LUTBits{ 0 };
    alignas( 8 ) std::array<Entry, ( 1UL << MAX_CODE_LENGTH )> m_codeCache{};
    bool m_needsToBeZeroed{ false };
};
}  // namespace rapidgzip
