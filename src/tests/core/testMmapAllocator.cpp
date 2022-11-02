#include <iostream>

#include <common.hpp>
#include <MmapAllocator.hpp>
#include <TestHelpers.hpp>


void
testMmapAllocator()
{
    MmapAllocator<int> allocator;
    auto* const pointer = allocator.allocate( 1 );
    REQUIRE( pointer != nullptr );
    *pointer = 3;
    allocator.deallocate( pointer, 1 );
}


int main()
{
    testMmapAllocator();

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors;
}
