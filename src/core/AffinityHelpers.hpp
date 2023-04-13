#pragma once


#ifndef __linux__

inline void
pinThreadToLogicalCore( int logicalCoreId )
{
    /** @todo */
}

[[nodiscard]] unsigned int
availableCores();

#else

[[nodiscard]] int
getRequiredBitMaskSize();

/**
 * Pins the calling thread to the given logical core / processing unit / hardware thread.
 */
void
pinThreadToLogicalCore( int logicalCoreId );

[[nodiscard]] unsigned int
availableCores();

#endif
