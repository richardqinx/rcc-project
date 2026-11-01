#ifndef LIBBASE_PRIVATE_CONFIG_H
#define LIBBASE_PRIVATE_CONFIG_H

// Include this header only under the libbase source tree.
// This is a private header.

/* Exported configuration */
#include "core/config/libbase-config.h"

/* Define to 1 to enable backtraces, and to 0 otherwise. */
#cmakedefine01 ENABLE_BACKTRACES

/* Define to 1 to enable crash overrides, and to 0 otherwise. */
#cmakedefine01 ENABLE_CRASH_OVERRIDES

/* Define to 1 to enable crash memory dumps, and to 0 otherwise. */
#cmakedefine01 RCC_ENABLE_CRASH_DUMPS

/* Define to 1 to prefer forward slashes on Windows, and to 0 otherwise. */
#cmakedefine01 RCC_WINDOWS_PREFER_FORWARD_SLASH

/* Define to 1 if you have the `backtrace' function. */
#cmakedefine HAVE_BACKTRACE ${HAVE_BACKTRACE}

#define BACKTRACE_HEADER <${BACKTRACE_HEADER}>

/* can use __crashreporter_info__ */
#cmakedefine01 HAVE_CRASHREPORTER_INFO

/* Define to 1 if you have the declaration of `arc4random', and to 0 if you
   don't. */
#cmakedefine01 HAVE_DECL_ARC4RANDOM

/* Define to 1 if you have the declaration of `strerror_s', and to 0 if you
   don't. */
#cmakedefine01 HAVE_DECL_STRERROR_S

/* Define if dlopen() is available on this platform. */
#cmakedefine HAVE_DLOPEN ${HAVE_DLOPEN}

/* Define to 1 if you have the `futimens' function. */
#cmakedefine HAVE_FUTIMENS ${HAVE_FUTIMENS}

/* Define to 1 if you have the `futimes' function. */
#cmakedefine HAVE_FUTIMES ${HAVE_FUTIMES}

/* Define to 1 if you have the `getpagesize' function. */
#cmakedefine HAVE_GETPAGESIZE ${HAVE_GETPAGESIZE}

/* Define to 1 if you have the `getrusage' function. */
#cmakedefine HAVE_GETRUSAGE ${HAVE_GETRUSAGE}

/* Define to 1 if you have the `isatty' function. */
#cmakedefine HAVE_ISATTY 1

/* Define to 1 if you have the `edit' library (-ledit). */
#cmakedefine HAVE_LIBEDIT ${HAVE_LIBEDIT}

/* Define to 1 if you have the `psapi' library (-lpsapi). */
#cmakedefine HAVE_LIBPSAPI ${HAVE_LIBPSAPI}

/* Define to 1 if you have the `pthread' library (-lpthread). */
#cmakedefine HAVE_LIBPTHREAD ${HAVE_LIBPTHREAD}

/* Define to 1 if you have the `pthread_getname_np' function. */
#cmakedefine HAVE_PTHREAD_GETNAME_NP ${HAVE_PTHREAD_GETNAME_NP}

/* Define to 1 if you have the `pthread_setname_np' function. */
#cmakedefine HAVE_PTHREAD_SETNAME_NP ${HAVE_PTHREAD_SETNAME_NP}

/* Define to 1 if you have the `pthread_get_name_np' function. */
#cmakedefine HAVE_PTHREAD_GET_NAME_NP ${HAVE_PTHREAD_GET_NAME_NP}

/* Define to 1 if you have the `pthread_set_name_np' function. */
#cmakedefine HAVE_PTHREAD_SET_NAME_NP ${HAVE_PTHREAD_SET_NAME_NP}

/* Define to 1 if you have the <mach/mach.h> header file. */
#cmakedefine HAVE_MACH_MACH_H ${HAVE_MACH_MACH_H}

/* Define to 1 if you have the `mallctl' function. */
#cmakedefine HAVE_MALLCTL ${HAVE_MALLCTL}

/* Define to 1 if you have the `mallinfo' function. */
#cmakedefine HAVE_MALLINFO ${HAVE_MALLINFO}

/* Define to 1 if you have the `mallinfo2' function. */
#cmakedefine HAVE_MALLINFO2 ${HAVE_MALLINFO2}

/* Define to 1 if you have the <malloc/malloc.h> header file. */
#cmakedefine HAVE_MALLOC_MALLOC_H ${HAVE_MALLOC_MALLOC_H}

/* Define to 1 if you have the `malloc_zone_statistics' function. */
#cmakedefine HAVE_MALLOC_ZONE_STATISTICS ${HAVE_MALLOC_ZONE_STATISTICS}

/* Define to 1 if you have the `posix_spawn' function. */
#cmakedefine HAVE_POSIX_SPAWN ${HAVE_POSIX_SPAWN}

/* Define to 1 if you have the `pread' function. */
#cmakedefine HAVE_PREAD ${HAVE_PREAD}

/* Define to 1 if you have the <pthread.h> header file. */
#cmakedefine HAVE_PTHREAD_H ${HAVE_PTHREAD_H}

/* Have pthread_rwlock_init */
#cmakedefine HAVE_PTHREAD_RWLOCK_INIT ${HAVE_PTHREAD_RWLOCK_INIT}

/* Define to 1 if you have the `sbrk' function. */
#cmakedefine HAVE_SBRK ${HAVE_SBRK}

/* Define to 1 if you have the `setenv' function. */
#cmakedefine HAVE_SETENV ${HAVE_SETENV}

/* Define to 1 if you have the `sigaltstack' function. */
#cmakedefine HAVE_SIGALTSTACK ${HAVE_SIGALTSTACK}

/* Define to 1 if you have the `strerror_r' function. */
#cmakedefine HAVE_STRERROR_R ${HAVE_STRERROR_R}

/* Define to 1 if you have the `sysconf' function. */
#cmakedefine HAVE_SYSCONF ${HAVE_SYSCONF}

/* Define to 1 if you have the <sys/mman.h> header file. */
#cmakedefine HAVE_SYS_MMAN_H ${HAVE_SYS_MMAN_H}

/* Define to 1 if you have the <sys/ioctl.h> header file. */
#cmakedefine HAVE_SYS_IOCTL_H ${HAVE_SYS_IOCTL_H}

/* Define to 1 if stat struct has st_mtimespec member .*/
#cmakedefine HAVE_STRUCT_STAT_ST_MTIMESPEC_TV_NSEC ${HAVE_STRUCT_STAT_ST_MTIMESPEC_TV_NSEC}

/* Define to 1 if stat struct has st_mtim member. */
#cmakedefine HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC ${HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC}

/* Define to 1 if you have the <unistd.h> header file. */
#cmakedefine HAVE_UNISTD_H ${HAVE_UNISTD_H}

/* Have host's _alloca */
#cmakedefine HAVE__ALLOCA ${HAVE__ALLOCA}

/* Define to 1 if you have the `_chsize_s' function. */
#cmakedefine HAVE__CHSIZE_S ${HAVE__CHSIZE_S}

/* Define to 1 if you have the `_Unwind_Backtrace' function. */
#cmakedefine HAVE__UNWIND_BACKTRACE ${HAVE__UNWIND_BACKTRACE}

/* Have host's __alloca */
#cmakedefine HAVE___ALLOCA ${HAVE___ALLOCA}

/* Have host's __ashldi3 */
#cmakedefine HAVE___ASHLDI3 ${HAVE___ASHLDI3}

/* Have host's __ashrdi3 */
#cmakedefine HAVE___ASHRDI3 ${HAVE___ASHRDI3}

/* Have host's __chkstk */
#cmakedefine HAVE___CHKSTK ${HAVE___CHKSTK}

/* Have host's __chkstk_ms */
#cmakedefine HAVE___CHKSTK_MS ${HAVE___CHKSTK_MS}

/* Have host's __cmpdi2 */
#cmakedefine HAVE___CMPDI2 ${HAVE___CMPDI2}

/* Have host's __divdi3 */
#cmakedefine HAVE___DIVDI3 ${HAVE___DIVDI3}

/* Have host's __fixdfdi */
#cmakedefine HAVE___FIXDFDI ${HAVE___FIXDFDI}

/* Have host's __fixsfdi */
#cmakedefine HAVE___FIXSFDI ${HAVE___FIXSFDI}

/* Have host's __floatdidf */
#cmakedefine HAVE___FLOATDIDF ${HAVE___FLOATDIDF}

/* Have host's __lshrdi3 */
#cmakedefine HAVE___LSHRDI3 ${HAVE___LSHRDI3}

/* Have host's __main */
#cmakedefine HAVE___MAIN ${HAVE___MAIN}

/* Have host's __moddi3 */
#cmakedefine HAVE___MODDI3 ${HAVE___MODDI3}

/* Have host's __udivdi3 */
#cmakedefine HAVE___UDIVDI3 ${HAVE___UDIVDI3}

/* Have host's __umoddi3 */
#cmakedefine HAVE___UMODDI3 ${HAVE___UMODDI3}

/* Have host's ___chkstk */
#cmakedefine HAVE____CHKSTK ${HAVE____CHKSTK}

/* Have host's ___chkstk_ms */
#cmakedefine HAVE____CHKSTK_MS ${HAVE____CHKSTK_MS}

/* Define if ICU library is available */
#cmakedefine01 HAVE_ICU

/* Define if Windows vendored ICU is available */
#cmakedefine01 HAVE_WINDOWS_ICU

/* Define if iconv library is available */
#cmakedefine01 HAVE_ICONV

/* Define to a function implementing stricmp */
#cmakedefine stricmp ${stricmp}

/* Define to a function implementing strdup */
#cmakedefine strdup ${strdup}

/* Whether Timers signpost passes in Xcode Instruments */
#cmakedefine01 RCC_SUPPORT_XCODE_SIGNPOSTS

#cmakedefine HAVE_PROC_PID_RUSAGE 1

#cmakedefine HAVE_GETAUXVAL ${HAVE_GETAUXVAL}

#endif
