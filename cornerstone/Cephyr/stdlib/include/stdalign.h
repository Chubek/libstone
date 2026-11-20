#ifndef _CEPHYR_STDALIGN_H
#define _CEPHYR_STDALIGN_H

#define alignof _Alignof
#define __alignof_is_defined 1
#if defined(__CEPHYR__)
#define __CEPHYR_ALIGNAS_UNAVAILABLE 1
#else
#define alignas _Alignas
#define __alignas_is_defined 1
#endif

#endif
