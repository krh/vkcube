AC_INIT([vkcube], [0.1])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AM_SILENT_RULES(yes)
LT_INIT

AC_PROG_CXX

PKG_PROG_PKG_CONFIG()

AC_ARG_WITH([minigbm],
    [AS_HELP_STRING([--with-minigbm@<:@=PATH@:>@],
                    [Path to minigbm repo for headers and libraries])])
if test "x$with_minigbm" != x; then
    MINIGBM_CPPFLAGS=-I$with_minigbm/usr/include
    MINIGBM_LDFLAGS="-L$with_minigbm/usr/lib -R$with_minigbm/usr/lib"
fi

AC_SUBST([MINIGBM_CPPFLAGS])
AC_SUBST([MINIGBM_LDFLAGS])

PKG_CHECK_MODULES([LIBDRM], [libdrm])
PKG_CHECK_MODULES([WAYLAND], [wayland-client])

AC_ARG_VAR([GLSLC], [path to glslc executable])
AC_SUBST([GLSLC])
AC_CHECK_PROGS([GLSLC], [glslc])
AC_CHECK_PROGS([XXD], [xxd])

AC_OUTPUT(Makefile)
