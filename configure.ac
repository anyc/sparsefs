# Sets up package and initializes build system.
AC_INIT([SparseFS], [0.2])
AC_CONFIG_SRCDIR([sparsefs.c])
AM_INIT_AUTOMAKE([foreign -Wall -Werror])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

# Checks for programs.
AC_PROG_CC

# Checks for libraries.
AC_CHECK_LIB([fuse], [fuse_main],, [AC_MSG_ERROR([You must have libfuse-dev installed to build sparsefs.])])

# Checks for packages which use pkg-config.
PKG_CHECK_MODULES([fuse], [fuse >= 2.6.0])

# Large file support
AC_SYS_LARGEFILE

CPPFLAGS="-D_FILE_OFFSET_BITS=64"

# Outputs resulting files.
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
