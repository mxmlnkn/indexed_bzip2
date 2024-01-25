#pragma once

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <VectorView.hpp>

#include "InflateWrapper.hpp"
#ifdef WITH_ISAL
    #include "isal.hpp"
#endif
#include "zlib.hpp"


enum class CompressionType : uint8_t
{
    NONE      =  0,
    DEFLATE   =  1,
    ZLIB      =  2,
    GZIP      =  3,
    BZIP2     =  4,
    LZ4       =  5,
    ZSTANDARD =  6,
    LZMA      =  7,
    XZ        =  8,
    BROTLI    =  9,
    LZIP      = 10,
    LZOP      = 11,
};


[[nodiscard]] const char*
toString( const CompressionType compressionType )
{
    switch ( compressionType )
    {
    case CompressionType::NONE     : return "NONE";
    case CompressionType::DEFLATE  : return "Deflate";
    case CompressionType::ZLIB     : return "ZLIB";
    case CompressionType::GZIP     : return "GZIP";
    case CompressionType::BZIP2    : return "BZIP2";
    case CompressionType::LZ4      : return "LZ4";
    case CompressionType::ZSTANDARD: return "ZStandard";
    case CompressionType::LZMA     : return "LZMA";
    case CompressionType::XZ       : return "XZ";
    case CompressionType::BROTLI   : return "Brotli";
    case CompressionType::LZIP     : return "LZIP";
    case CompressionType::LZOP     : return "LZOP";
    }
    return "Unknown";
}


/**
 * The methods by design are not called simply "data"/"size" to avoid it being used the wrong way
 * when replacing nomal containers for this one.
 */
template<typename Container = std::vector<uint8_t> >
class CompressedVector
{
public:
    using container_type = Container;

public:
    CompressedVector() = default;

    explicit
    CompressedVector( const VectorView<typename Container::value_type> toCompress,
                      const CompressionType                            compressionType ) :
        m_compressionType( compressionType ),
        m_decompressedSize( toCompress.size() )
    {
        if ( m_compressionType != CompressionType::GZIP ) {
            throw std::invalid_argument( std::string( "Only gzip compression is currently supported but got: " )
                                         + toString( m_compressionType ) );
        }

    #ifdef WITH_ISAL
        try {
            m_data = rapidgzip::compressWithIsal<Container>( toCompress );
        } catch ( const std::runtime_error& exception ) {
            std::cerr << "[Warning] Compression with ISA-L failed unexpectedly with: " << exception.what() << "\n";
            std::cerr << "[Warning] Will use zlib as a fallback. Please report this bug anyway.\n";
            m_data = rapidgzip::compressWithZlib<Container>( toCompress );
        }
    #else
        m_data = rapidgzip::compressWithZlib<Container>( toCompress );
    #endif
    }

    CompressedVector( container_type        data,
                      size_t                decompressedSize,
                      const CompressionType compressionType ) :
        m_compressionType( compressionType ),
        m_data( std::move( data ) ),
        m_decompressedSize( decompressedSize )
    {
        if ( m_compressionType != CompressionType::GZIP ) {
            throw std::invalid_argument( std::string( "Only gzip compression is currently supported but got: " )
                                         + toString( m_compressionType ) );
        }
    }

    [[nodiscard]] CompressionType
    compressionType() const noexcept
    {
        return m_compressionType;
    }

    [[nodiscard]] const Container&
    compressedData() const noexcept
    {
        return m_data;
    }

    [[nodiscard]] size_t
    compressedSize() const noexcept
    {
        return m_data.size();
    }

    [[nodiscard]] Container
    decompress() const
    {
        if ( empty() ) {
            return {};
        }

    #ifdef WITH_ISAL
        return inflateWithWrapper<rapidgzip::IsalInflateWrapper>( m_data, m_decompressedSize );
    #else
        return inflateWithWrapper<rapidgzip::ZlibInflateWrapper>( m_data, m_decompressedSize );
    #endif
    }

    [[nodiscard]] size_t
    decompressedSize() const noexcept
    {
        return m_decompressedSize;
    }

    void
    clear()
    {
        m_data.clear();
        m_decompressedSize = 0;
    }

    [[nodiscard]] bool
    empty() const noexcept
    {
        return m_decompressedSize == 0;
    }

    [[nodiscard]] bool
    operator==( const CompressedVector& other ) const
    {
        return ( m_compressionType == other.m_compressionType )
               && ( m_data == other.m_data )
               && ( m_decompressedSize == other.m_decompressedSize );
    }

private:
    CompressionType m_compressionType{ CompressionType::GZIP };
    Container m_data;
    size_t m_decompressedSize{ 0 };
};

