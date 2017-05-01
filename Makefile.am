AM_CPPFLAGS =					\
	$(MESA_CPPFLAGS)			\
	-D_POSIX_C_SOURCE=200809L

common_CFLAGS = -Wall -O0 -g3

AM_CFLAGS = -std=c11				\
	 $(common_CFLAGS)			\
	-Werror=implicit-function-declaration	\
	-Werror=missing-prototypes

AM_CXXFLAGS = -std=c++11 $(common_CFLAGS)

noinst_PROGRAMS = vkcube

BUILT_SOURCES = \
	vkcube.vert.spv.h			\
	vkcube.frag.spv.h

%.spv.h: %
	@if test -z "$(GLSLC)"; then echo "Missing glslc. See https://github.com/google/shaderc"; exit 1; fi
	$(AM_GEN) $(GLSLC) -o - $< | xxd -i > $@.tmp && mv $@.tmp $@

vkcube_SOURCES =				\
	$(BUILT_SOURCES)			\
	xdg-shell-unstable-v6-protocol.c	\
	main.c					\
	common.h				\
	cube.c					\
	esTransform.c				\
	esUtil.h

CLEANFILES = $(BUILT_SOURCES)

vkcube_CFLAGS = $(AM_CFLAGS) $(MINIGBM_CPPFLAGS) $(LIBDRM_CFLAGS) $(WAYLAND_CFLAGS)  $(LIBPNG_CFLAGS)
vkcube_LDADD = $(MINIGBM_LDFLAGS) $(MESA_LDFLAGS) $(LIBDRM_LIBS) $(WAYLAND_LIBS)  $(LIBPNG_LIBS) -lgbm -lm -lvulkan -lxcb
