dnl
dnl Check for a system liblmdb; if not available or disabled, use our own
dnl embedded copy of liblmdb.
dnl
dnl Results in the following subst vars:
dnl
dnl - @AFSLMDB_ONLY@: Set to the empty string when we are using our embedded
dnl copy of liblmdb. Otherwise set to '#', so Makefile lines prefixed with
dnl @AFSLMDB_ONLY@ are ignored.
dnl
dnl - @LMDB_CFLAGS@, @LMDB_LIBS@: the compiler and linker args to use when
dnl building something with lmdb support.
dnl
AC_DEFUN([OPENAFS_LMDB],
 [
  AC_ARG_WITH([lmdb],
   [AS_HELP_STRING([--with-lmdb],
     [enable/disable system liblmdb (defaults to autodetect)])],
   [openafs_system_lmdb="$withval"])

  AS_IF([test x"$openafs_system_lmdb" = x],
   [PKG_CHECK_EXISTS([lmdb], [openafs_system_lmdb=yes])])

  AS_IF([test x"$openafs_system_lmdb" = xyes],
   [PKG_CHECK_MODULES([LMDB], [lmdb],
     [],
     [AC_MSG_ERROR(["$LMDB_PACKAGE_ERRORS"])])],
   [openafs_system_lmdb=no])

  AFSLMDB_ONLY='#'
  AS_IF([test x"$openafs_system_lmdb" = xno],
   [AC_MSG_NOTICE([Using embedded copy of liblmdb])
    AFSLMDB_ONLY=
    LMDB_CFLAGS=
    LMDB_LIBS=])

  AC_SUBST([AFSLMDB_ONLY])
  AC_SUBST([LMDB_CFLAGS])
  AC_SUBST([LMDB_LIBS])
])
