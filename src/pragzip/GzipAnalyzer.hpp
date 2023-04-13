#pragma once

#include <filereader/FileReader.hpp>

#include "Error.hpp"


namespace pragzip::deflate
{
[[nodiscard]] pragzip::Error
analyze( UniqueFileReader inputFile );
}  // namespace pragzip::deflate
