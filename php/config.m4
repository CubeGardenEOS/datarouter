PHP_ARG_WITH([datarouter], [DataRouter checkout], [AS_HELP_STRING([--with-datarouter=DIR], [Build DataRouter extension against DIR/build])], [no])
if test "$PHP_DATAROUTER" != "no"; then
  AC_CHECK_HEADER([$PHP_DATAROUTER/include/datarouter_frontend.h], [], [AC_MSG_ERROR([missing frontend header])])
  PHP_ADD_INCLUDE([$PHP_DATAROUTER/include])
  PHP_ADD_LIBRARY_WITH_PATH([blinkcms_datarouter], [$PHP_DATAROUTER/build], [DATAROUTER_SHARED_LIBADD])
  PHP_SUBST([DATAROUTER_SHARED_LIBADD])
  PHP_NEW_EXTENSION([datarouter], [datarouter.c], [$ext_shared])
fi
