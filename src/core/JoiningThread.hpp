#pragma once

#include <thread>
#include <utility>

#ifdef WITH_RPMALLOC
    #include <rpmalloc.h>
#endif


#ifdef WITH_RPMALLOC
class RpmallocThreadInit
{
public:
    RpmallocThreadInit()
    {
        rpmalloc_thread_initialize();
    }

    ~RpmallocThreadInit()
    {
        rpmalloc_thread_finalize( /* release caches */ true );
    }
};
#endif


/**
 * Similar to the planned C++20 std::jthread, this class joins in the destructor.
 * Additionally, it ensures that all threads created with this interface correctly initialize rpmalloc!
 */
class JoiningThread
{
public:
    template<class Function, class... Args>
    explicit
    JoiningThread( Function&& function, Args&&... args ) :
#ifdef WITH_RPMALLOC
        m_thread( [=] () {
            static const thread_local RpmallocThreadInit rpmallocThreadInit{};
            function( std::forward<Args>( args )... );
        } )
#else
        m_thread( std::forward<Function>( function ), std::forward<Args>( args )... )
#endif
    {}

    JoiningThread( JoiningThread&& ) = default;
    JoiningThread( const JoiningThread& ) = delete;
    JoiningThread& operator=( JoiningThread&& ) = delete;
    JoiningThread& operator=( const JoiningThread& ) = delete;

    ~JoiningThread()
    {
        if ( m_thread.joinable() ) {
            m_thread.join();
        }
    }

    [[nodiscard]] std::thread::id
    get_id() const noexcept
    {
        return m_thread.get_id();
    }

    [[nodiscard]] bool
    joinable() const
    {
        return m_thread.joinable();
    }

    void
    join()
    {
        m_thread.join();
    }

private:
    std::thread m_thread;
};
