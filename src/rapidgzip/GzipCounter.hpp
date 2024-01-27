#pragma once

#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <stdexcept>
#include <utility>

#include <filereader/FileReader.hpp>

#include "deflate.hpp"
#include "Error.hpp"


namespace rapidgzip::deflate
{
[[nodiscard]] std::pair<size_t, rapidgzip::Error>
countDecompressedBytes( UniqueFileReader inputFile )
{
    using namespace rapidgzip;
    using Block = rapidgzip::deflate::Block</* Statistics */ true>;

    rapidgzip::BitReader bitReader{ std::move( inputFile ) };

    std::optional<gzip::Header> gzipHeader;
    Block block;

    size_t totalBytesRead = 0;
    size_t streamBytesRead = 0;

    while ( true ) {
        if ( !gzipHeader ) {
            const auto [header, error] = gzip::readHeader( bitReader );
            if ( error != Error::NONE ) {
                std::cerr << "Encountered error: " << toString( error )
                          << " while trying to read gzip header!\n";
                return { 0, error };
            }

            gzipHeader = header;
            block.setInitialWindow();
        }

        /* Read deflate header. */
        {
            const auto error = block.readHeader( bitReader );
            if ( error != Error::NONE ) {
                std::cerr << "Encountered error: " << toString( error )
                          << " while trying to read deflate header!\n";
                return { 0, error };
            }
        }

        while ( !block.eob() ) {
            const auto [buffers, error] = block.read( bitReader, std::numeric_limits<size_t>::max() );
            const auto nBytesRead = buffers.size();
            if ( error != Error::NONE ) {
                std::cerr << "Encountered error: " << toString( error )
                          << " while decompressing deflate block.\n";
            }
            totalBytesRead += nBytesRead;
            streamBytesRead += nBytesRead;
        }

        if ( block.isLastBlock() ) {
            const auto footer = gzip::readFooter( bitReader );

            if ( static_cast<uint32_t>( streamBytesRead ) != footer.uncompressedSize ) {
                std::stringstream message;
                message << "Mismatching size (" << static_cast<uint32_t>( streamBytesRead )
                        << " <-> footer: " << footer.uncompressedSize << ") for gzip stream!";
                throw std::runtime_error( std::move( message ).str() );
            }

            gzipHeader = {};
        }

        if ( bitReader.eof() ) {
            break;
        }
    }

    return { totalBytesRead, Error::NONE };
}
}  // namespace rapidgzip::deflate
