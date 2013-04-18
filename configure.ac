AC_PREREQ([2.61])dnl Possibly earlier will do, but this is what I have
AC_INIT([obfsproxy], [0.0])
AC_CONFIG_SRCDIR([src/main.c])
AM_INIT_AUTOMAKE([foreign nostdinc])

### Programs ###

AC_PROG_CC
AC_PROG_RANLIB
AC_PROG_SED

AM_PATH_PYTHON([2.6],, [:])
AM_CONDITIONAL([HAVE_PYTHON], [test "$PYTHON" != ":"])
if test "$PYTHON" == ":"; then
  AC_MSG_WARN([Python interpreter not found; integration tests disabled.])
fi

PKG_PROG_PKG_CONFIG

### Headers ###

AC_CHECK_HEADERS([sys/socket.h])
AC_CHECK_HEADERS([sys/types.h])
AC_CHECK_HEADERS([netinet/in.h])

### Libraries ###

PKG_CHECK_MODULES([libevent], [libevent >= 2.0])
# Presently no need for libssl, only libcrypto.
PKG_CHECK_MODULES([libcrypto], [libcrypto >= 0.9.7])

# We permit the use of openssl 0.9.7, which doesn't have sha256.
# Check whether a replacement is required.
save_LIBS="$LIBS"
LIBS="$libcrypto_LIBS"
AC_CHECK_FUNC(SHA256_Init, [:], [:])
LIBS="$save_LIBS"
AM_CONDITIONAL(NEED_SHA256, [test x$ac_cv_func_SHA256_Init = xno])

# ntohl and a bunch of related functions require a special library on Windows.
# It is possible that libevent or libcrypto has hooked us up already.
# This can't be done with AC_SEARCH_LIBS -- see m4/winsock.m4 for gory details.
save_LIBS="$LIBS"
LIBS="$libevent_LIBS $libcrypto_LIBS"
AX_LIB_WINSOCK2
LIBS="$save_LIBS"

### C features ###

AC_C_INLINE
AC_CHECK_SIZEOF(int)
AC_CHECK_SIZEOF(size_t)

dnl check for working va_copy. It could be a function or a macro.
AC_MSG_CHECKING([whether we have va_copy])
AC_CACHE_VAL(obfs_cv_have_vacopy,
  AC_TRY_LINK([#include <stdarg.h>], [
    va_list ap, ap2;
    va_copy(ap, ap2);
  ], [obfs_cv_have_vacopy=true],
     [obfs_cv_have_vacopy=false]))
if test "$obfs_cv_have_vacopy" = true; then
  AC_DEFINE(HAVE_VA_COPY, 1, [True if this platform has the standard va_copy macro])
  AC_MSG_RESULT(yes)
else
  AC_MSG_RESULT(no)
fi

### Output ###

AC_CONFIG_FILES([Makefile])
AC_CONFIG_HEADERS([config.h])
AC_OUTPUT
