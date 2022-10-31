#pragma once

#include <limits>
#include <new>
#include <type_traits>
#include <vector>

#ifdef __linux__

#include <sys/mman.h>


/**
 * Allocates with mmap and deallocates with munmap. Data allocated as such can be used with vmsplice.
 * The standard allocator won't do because deallocating it might not call munmap and instead it might
 * get reused leading to wrong data written out with vmsplice.
 */
template<typename ElementType>
class MmapAllocator
{
public:
    using value_type = ElementType;

    [[nodiscard]] constexpr ElementType*
    allocate( std::size_t nElementsToAllocate )
    {
        // Mostly 32 KiB for base64.gz
        //std::cerr << "Allocate " << nElementsToAllocate * sizeof( ElementType ) / 1024 << " KiB\n";

        auto const nBytesToAllocate = nElementsToAllocate * sizeof( ElementType );
        auto* const result = mmap( nullptr, nBytesToAllocate, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                                   /* fd */ -1, /* offset */ 0 );
        if ( result == (void*)-1 ) {
            throw std::bad_alloc();
        }
        return reinterpret_cast<ElementType*>( result );
    }

    constexpr void
    deallocate( ElementType* allocatedPointer,
                std::size_t  nBytesAllocated )
    {
        munmap( allocatedPointer, nBytesAllocated );
    }
};


template<typename T>
using SplicableVector = std::vector<T, MmapAllocator<T> >;

#else

template<typename T>
using SplicableVector = std::vector<T>;

#endif
