/***************************************************************************
 *   CK803S (C-SKY V2) atomic ops for Miosix.                              *
 *   Verbatim port of atomic_ops_impl_cortexM0.h (Terraneo Federico):      *
 *   CK803S has no LL/SC exposed to us here, so atomics are implemented    *
 *   by briefly disabling interrupts (rides on doDisableInterrupts via     *
 *   InterruptDisableLock). Same approach the Cortex-M0 port uses.         *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version. As a special exception, linking   *
 *   this file with other works does not by itself cause the resulting     *
 *   work to be covered by the GPL.                                        *
 ***************************************************************************/

#ifndef ATOMIC_OPS_IMPL_CSKYV2_H
#define ATOMIC_OPS_IMPL_CSKYV2_H

#include "interfaces/arch_registers.h"
#include <kernel/kernel.h>

namespace miosix {

inline int atomicSwap(volatile int *p, int v)
{
    InterruptDisableLock dLock;
    int result = *p;
    *p = v;
    return result;
}

inline void atomicAdd(volatile int *p, int incr)
{
    InterruptDisableLock dLock;
    *p += incr;
}

inline int atomicAddExchange(volatile int *p, int incr)
{
    InterruptDisableLock dLock;
    int result = *p;
    *p += incr;
    return result;
}

inline int atomicCompareAndSwap(volatile int *p, int prev, int next)
{
    InterruptDisableLock dLock;
    int result = *p;
    if(*p == prev) *p = next;
    return result;
}

inline void *atomicFetchAndIncrement(void * const volatile * p, int offset,
        int incr)
{
    InterruptDisableLock dLock;
    volatile uint32_t *pt;
    void *result = const_cast<void*>(*p);
    if(result == 0) return 0;
    pt = reinterpret_cast<uint32_t*>(result) + offset;
    *pt += incr;
    return result;
}

} //namespace miosix

#endif //ATOMIC_OPS_IMPL_CSKYV2_H
