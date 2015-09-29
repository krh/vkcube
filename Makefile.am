CPPFLAGS =					\
	$(MESA_CPPFLAGS)			\
	-D_POSIX_C_SOURCE=200809L

common_CFLAGS = -Wall -O0 -g3

CFLAGS = -std=c11				\
	 $(common_CFLAGS)			\
	-Werror=implicit-function-declaration	\
	-Werror=missing-prototypes

CXXFLAGS = -std=c++11 $(common_CFLAGS)

noinst_PROGRAMS = vkcube

BUILT_SOURCES = \
	vkcube.vert.spv.h			\
	vkcube.frag.spv.h

%.spv.h: %
	$(GLSLC) -o - $< | xxd -i > $@

vkcube_SOURCES =				\
	$(BUILT_SOURCES)			\
	xdg-shell-protocol.c			\
	vkcube.c				\
	esTransform.c				\
	esUtil.h

CLEANFILES = $(BUILT_SOURCES)

vkcube_CFLAGS = $(CFLAGS) $(MINIGBM_CPPFLAGS) $(LIBDRM_CFLAGS) $(WAYLAND_CFLAGS)
vkcube_LDADD = $(MINIGBM_LDFLAGS) $(MESA_LDFLAGS) $(LIBDRM_LIBS) $(WAYLAND_LIBS) -lgbm -lm -lvulkan -lpng16 -lxcb
