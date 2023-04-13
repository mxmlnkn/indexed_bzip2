#include "BitManipulation.hpp"

#include <array>
#include <limits>
#include <type_traits>


template<typename T>
[[nodiscard]] constexpr auto
createReversedBitsLUT()
{
    static_assert( std::is_unsigned_v<T> && std::is_integral_v<T> );

    std::array<T, 1ULL << std::numeric_limits<T>::digits> result{};
    for ( size_t i = 0; i < result.size(); ++i ) {
        result[i] = reverseBitsWithoutLUT( static_cast<T>( i ) );
    }
    return result;
}


/**
 * This intermediary constexpr variable forces createReversedBitsLUT to be evaluated at compile-time
 * to ensure that REVERSED_BITS_LUT is value-initialized instead of calling a function and program start.
 */
template<typename T>
constexpr auto REVERSED_BITS_LUT_COMPILE_TIME = createReversedBitsLUT<T>();

template<> std::array<uint8_t, 0x100ULL> REVERSED_BITS_LUT<uint8_t> = REVERSED_BITS_LUT_COMPILE_TIME<uint8_t>;
template<> std::array<uint16_t, 0x10000ULL> REVERSED_BITS_LUT<uint16_t> = REVERSED_BITS_LUT_COMPILE_TIME<uint16_t>;
