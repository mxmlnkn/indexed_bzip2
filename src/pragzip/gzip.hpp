#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "definitions.hpp"      // gzip::BitReader
#include "Error.hpp"


namespace pragzip::gzip
{
/** For this namespace, refer to @see RFC 1952 "GZIP File Format Specification" */

constexpr auto MAGIC_ID1 = 0x1FU;
constexpr auto MAGIC_ID2 = 0x8BU;
constexpr auto MAGIC_COMPRESSION = 0x08U;

/* Note that the byte order is reversed because of the LSB BitReader. */
constexpr auto MAGIC_BYTES_GZIP = 0x08'8B'1FU;

/* This is not a gzip-specific constant. It's such so that the decoder will not try to
 * read the whole file to memory for invalid data. */
constexpr auto MAX_ALLOWED_FIELD_SIZE = 1024 * 1024;


[[nodiscard]] std::string
getOperatingSystemName( uint8_t code ) noexcept;

[[nodiscard]] std::string
getExtraFlagsDescription( uint8_t code ) noexcept;


struct Header
{
    uint32_t modificationTime{ 0 };
    uint8_t operatingSystem{ 0 };
    /**
     * 2: compressor used maximum compression, slowest algorithm
     * 4: compressor used fastest algorithm
     */
    uint8_t extraFlags{ 0 };

    bool isLikelyASCII{ false };
    std::optional<std::vector<uint8_t> > extra;
    std::optional<std::string> fileName;
    std::optional<std::string> comment;
    std::optional<uint16_t> crc16;
};


struct Footer
{
    uint32_t crc32{ 0 };
    uint32_t uncompressedSize{ 0 };  // If larger than UINT32_MAX, then contains the modulo.
};


std::pair<Header, Error>
readHeader( BitReader& bitReader );

Error
checkHeader( BitReader& bitReader );

Footer
readFooter( BitReader& bitReader );
} // namespace pragzip::gzip
