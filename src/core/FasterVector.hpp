#pragma once

#include <vector>


// Allocator adaptor that interposes construct() calls to
// convert value initialization into default initialization.
template <typename T, typename A = std::allocator<T> >
class NonInitializingAllocator :
    public A
{
private:
    using a_t = std::allocator_traits<A>;

public:
    template <typename U>
    struct rebind
    {
        using other = NonInitializingAllocator<U, typename a_t::template rebind_alloc<U> >;
    };

    using A::A;

    template<typename U>
    void construct( U* ptr ) noexcept( std::is_nothrow_default_constructible<U>::value )
    {
        if constexpr ( !std::is_pod_v<T> ) {
            ::new( static_cast<void*>( ptr ) ) U;
        }
    }

    //template<typename U, typename... Args>
    //void
    //construct( U* ptr, Args&&... args )
    //{
    //    a_t::construct( static_cast<A&>( *this ), ptr, std::forward<Args>( args )... );
    //}
};

#ifdef WITH_RPMALLOC
    #include <rpmalloc.h>


class RpmallocInit
{
public:
    RpmallocInit()
    {
        rpmalloc_initialize();
    }

    ~RpmallocInit()
    {
        rpmalloc_finalize();
    }
};


/* It must be the very first static variable so that rpmalloc is initialized before any usage of malloc
 * when overriding operator new (when including rpnew.h). And, this only works if everything is header-only
 * because else the static variable initialization order becomes undefined across different compile units.
 * That's why we avoid overriding operators new and delete and simply use it as a custom allocator in the
 * few places we know to be performance-critical */
inline static const RpmallocInit rpmallocInit{};


template<typename ElementType>
class RpmallocAllocator
{
public:
    using value_type = ElementType;

    using is_always_equal = std::true_type;

public:
    [[nodiscard]] constexpr ElementType*
    allocate( std::size_t nElementsToAllocate )
    {
        if ( nElementsToAllocate > std::numeric_limits<std::size_t>::max() / sizeof( ElementType ) ) {
            throw std::bad_array_new_length();
        }

        auto const nBytesToAllocate = nElementsToAllocate * sizeof( ElementType );
        return reinterpret_cast<ElementType*>( rpmalloc( nBytesToAllocate ) );
    }

    constexpr void
    deallocate( ElementType*                 allocatedPointer,
                [[maybe_unused]] std::size_t nElementsAllocated )
    {
        rpfree( allocatedPointer );
    }

    /* I don't understand why this is still necessary even with is_always_equal = true_type.
     * Defining it to true type should be necessary because the default implementation of
     * is_always_equal == is_empty should also be true because this class does not have any members. */
    [[nodiscard]] bool
    operator==( const RpmallocAllocator& ) const
    {
        return true;
    }

    [[nodiscard]] bool
    operator!=( const RpmallocAllocator& ) const
    {
        return false;
    }

    //template<typename U>
    //void construct( U* ptr ) noexcept( std::is_nothrow_default_constructible<U>::value )
    //{
    //    ::new( static_cast<void*>( ptr ) ) U;
    //}

    //template<typename U, typename... Args>
    //void
    //construct( U* ptr, Args&&... args )
    //{
    //    a_t::construct( static_cast<A&>( *this ), ptr, std::forward<Args>( args )... );
    //}
};


static_assert( std::is_empty_v<RpmallocAllocator<char> > );


template<typename T>
using FasterVector = std::vector<T, RpmallocAllocator<T> >;

template<typename T>
using FasterVector2 = std::vector<T, NonInitializingAllocator<RpmallocAllocator<T> > >;

#else

template<typename T>
using FasterVector = std::vector<T>;

template<typename T>
using FasterVector2 = std::vector<T, NonInitializingAllocator<T> >;

#endif
