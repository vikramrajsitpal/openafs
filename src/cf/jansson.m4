dnl Jansson Autoconf glue.

AC_DEFUN([OPENAFS_JANSSON],
 [AC_ARG_WITH([jansson],
   [AS_HELP_STRING([--with-jansson],
		 [Force use of the jansson JSON library @<:@default=check@:>@])],
   [],
   [with_jansson=check])

  JANSSON_ONLY='#'
  AS_IF([test x"$with_jansson" != xno],
    [PKG_CHECK_MODULES([JANSSON], [jansson],
		      [AC_DEFINE([ENABLE_JANSSON], [1],
				 [Build with jansson JSON support])
		       JANSSON_ONLY=],
		      [AS_IF([test x"$with_jansson" != xcheck],
			     [AC_MSG_ERROR([$JANSSON_PKG_ERRORS])])])])
  AC_SUBST([JANSSON_CFLAGS])
  AC_SUBST([JANSSON_LIBS])
  AC_SUBST([JANSSON_ONLY])])
