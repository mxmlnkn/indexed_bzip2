add_subdirectory(huffman)
add_subdirectory(blockfinder)


add_library(libpragzip STATIC)
target_include_directories(libpragzip PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_sources(libpragzip PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/crc32.hpp
    ${CMAKE_CURRENT_LIST_DIR}/ChunkData.hpp
    ${CMAKE_CURRENT_LIST_DIR}/DecodedData.hpp
    ${CMAKE_CURRENT_LIST_DIR}/DecodedDataView.hpp
    ${CMAKE_CURRENT_LIST_DIR}/definitions.hpp
    ${CMAKE_CURRENT_LIST_DIR}/deflate.hpp
    ${CMAKE_CURRENT_LIST_DIR}/Error.hpp
    ${CMAKE_CURRENT_LIST_DIR}/FixedHuffmanCoding.cpp
    ${CMAKE_CURRENT_LIST_DIR}/FixedHuffmanCoding.hpp
    ${CMAKE_CURRENT_LIST_DIR}/gzip.hpp
    ${CMAKE_CURRENT_LIST_DIR}/GzipAnalyzer.hpp
    ${CMAKE_CURRENT_LIST_DIR}/GzipBlockFinder.hpp
    ${CMAKE_CURRENT_LIST_DIR}/GzipChunkFetcher.hpp
    ${CMAKE_CURRENT_LIST_DIR}/GzipReader.hpp
    ${CMAKE_CURRENT_LIST_DIR}/IndexFileFormat.hpp
    ${CMAKE_CURRENT_LIST_DIR}/MarkerReplacement.hpp
    ${CMAKE_CURRENT_LIST_DIR}/ParallelGzipReader.hpp
    ${CMAKE_CURRENT_LIST_DIR}/pragzip.hpp
    ${CMAKE_CURRENT_LIST_DIR}/precode.hpp
    ${CMAKE_CURRENT_LIST_DIR}/WindowMap.hpp
)
target_link_libraries(libpragzip PUBLIC libhuffman libblockfinder core zlibstatic)
target_compile_features(libpragzip PUBLIC cxx_std_17)
