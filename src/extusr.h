/*
 * EXTNT - base types for building the shared engine (ext*.c) outside the
 * kernel: the format/check/image tools and the Linux test harness. The
 * driver build uses ntifs.h instead (see ext.h).
 */

#ifndef _EXTUSR_H_
#define _EXTUSR_H_

#ifdef _WIN32

#include <windows.h>

#ifdef _MSC_VER
#define snprintf    _snprintf
#define vsnprintf   _vsnprintf
#endif

#else

#include <stdint.h>
#include <stddef.h>
typedef void            VOID, *PVOID;
typedef unsigned char   UCHAR, *PUCHAR, BOOLEAN, *PBOOLEAN;
typedef char            CHAR, *PCHAR;
typedef uint16_t        USHORT, *PUSHORT, WCHAR, *PWCHAR;
typedef int16_t         SHORT;
typedef uint32_t        ULONG, *PULONG;
typedef int32_t         LONG, *PLONG;
typedef uint64_t        ULONGLONG, *PULONGLONG;
typedef int64_t         LONGLONG;
typedef uintptr_t       ULONG_PTR;

#endif /* _WIN32 */

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#endif /* _EXTUSR_H_ */
