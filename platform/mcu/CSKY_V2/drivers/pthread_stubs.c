/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * No-op pthread_mutex stubs for the HD2 bring-up build.
 *
 * OpenRTX's peripherals/spi.h inlines call pthread_mutex_lock/unlock
 * etc. through a NULL-guard:
 *
 *     if (dev->mutex == NULL) return 0;
 *     return pthread_mutex_lock(dev->mutex);
 *
 * Even though every HD2 spi device passes mutex=NULL, the compiler
 * can't prove the field is constant across the cfg->spi indirection,
 * so it still emits unresolved references to the four functions
 * below.  Newlib supplies <pthread.h> declarations but no
 * implementation -- those normally come from the RTOS layer (miosix).
 *
 * For single-threaded bring-up these stubs are correct: every call
 * comes from code that's already gated by `mutex != NULL`, so this
 * file is reached only if someone *does* attach a real mutex, in
 * which case the stubs become a real bug.  When miosix lands and
 * provides a working pthread layer, delete this file.
 */

#include <pthread.h>

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    (void)m; (void)a; return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    (void)m; return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    (void)m; return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    (void)m; return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    (void)m; return 0;
}
