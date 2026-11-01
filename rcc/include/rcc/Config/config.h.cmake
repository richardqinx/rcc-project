#ifndef RCC_PRIVATE_CONFIG_H
#define RCC_PRIVATE_CONFIG_H

// Include this header only under the rcc source tree.
// This is a private header.

/* Exported configuration */
#include "rcc/Config/rcc-config.h"

/* Bug report URL. */
#define BUG_REPORT_URL "${BUG_REPORT_URL}"

/* Define to 1 if we can register EH frames on this platform. */
#cmakedefine HAVE_REGISTER_FRAME ${HAVE_REGISTER_FRAME}

/* Define to 1 if we can deregister EH frames on this platform. */
#cmakedefine HAVE_DEREGISTER_FRAME ${HAVE_DEREGISTER_FRAME}

/* Define if __unw_add_dynamic_fde() is available on this platform. */
#cmakedefine HAVE_UNW_ADD_DYNAMIC_FDE ${HAVE_UNW_ADD_DYNAMIC_FDE}

/* Define if libffi is available on this platform. */
#cmakedefine HAVE_FFI_CALL ${HAVE_FFI_CALL}

/* Define to 1 if you have the <ffi/ffi.h> header file. */
#cmakedefine HAVE_FFI_FFI_H ${HAVE_FFI_FFI_H}

/* Define to 1 if you have the <ffi.h> header file. */
#cmakedefine HAVE_FFI_H ${HAVE_FFI_H}

/* Define to 1 if you have the `pfm' library (-lpfm). */
#cmakedefine HAVE_LIBPFM ${HAVE_LIBPFM}

/* Define to 1 if the `perf_branch_entry' struct has field cycles. */
#cmakedefine LIBPFM_HAS_FIELD_CYCLES ${LIBPFM_HAS_FIELD_CYCLES}

/* Linker version detected at compile time. */
#cmakedefine HOST_LINK_VERSION "${HOST_LINK_VERSION}"

/* Define if overriding target triple is enabled */
#cmakedefine RCC_TARGET_TRIPLE_ENV "${RCC_TARGET_TRIPLE_ENV}"

/* Whether tools show host and target info when invoked with --version */
#cmakedefine01 RCC_VERSION_PRINTER_SHOW_HOST_TARGET_INFO

/* Whether tools show optional build config flags when invoked with --version */
#cmakedefine01 RCC_VERSION_PRINTER_SHOW_BUILD_CONFIG

/* Define if libxml2 is supported on this platform. */
#cmakedefine RCC_ENABLE_LIBXML2 ${RCC_ENABLE_LIBXML2}

/* Define to the extension used for shared libraries, say, ".so". */
#cmakedefine LTDL_SHLIB_EXT "${LTDL_SHLIB_EXT}"

/* Define to the extension used for plugin libraries, say, ".so". */
#cmakedefine RCC_PLUGIN_EXT "${RCC_PLUGIN_EXT}"

/* Define to the address where bug reports for this package should be sent. */
#cmakedefine PACKAGE_BUGREPORT "${PACKAGE_BUGREPORT}"

/* Define to the full name of this package. */
#cmakedefine PACKAGE_NAME "${PACKAGE_NAME}"

/* Define to the full name and version of this package. */
#cmakedefine PACKAGE_STRING "${PACKAGE_STRING}"

/* Define to the version of this package. */
#cmakedefine PACKAGE_VERSION "${PACKAGE_VERSION}"

/* Define to the vendor of this package. */
#cmakedefine PACKAGE_VENDOR "${PACKAGE_VENDOR}"

/* Whether GlobalISel rule coverage is being collected */
#cmakedefine01 RCC_GISEL_COV_ENABLED

/* Define to the default GlobalISel coverage file prefix */
#cmakedefine RCC_GISEL_COV_PREFIX "${RCC_GISEL_COV_PREFIX}"

/* Have host's builtin thread pointer. */
#cmakedefine HAVE_BUILTIN_THREAD_POINTER ${HAVE_BUILTIN_THREAD_POINTER}

#endif
