#pragma once

#include <algorithm>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <AffinityHelpers.hpp>
#include <BlockMap.hpp>
#include <common.hpp>
#include <filereader/FileReader.hpp>
#include <filereader/Shared.hpp>

#ifdef WITH_PYTHON_SUPPORT
    #include <filereader/Python.hpp>
    #include <filereader/Standard.hpp>
#endif

#include "crc32.hpp"
#include "GzipChunkFetcher.hpp"
#include "GzipBlockFinder.hpp"
#include "gzip.hpp"
#include "IndexFileFormat.hpp"


namespace pragzip
{
/**
 * @note Calls to this class are not thread-safe! Even though they use threads to evaluate them in parallel.
 */
template class ParallelGzipReader<ChunkData>;
}  // namespace pragzip
