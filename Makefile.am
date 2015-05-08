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

vkcube_SOURCES =				\
	vkcube.c				\
	esTransform.c				\
	esUtil.h

vkcube_CFLAGS = $(CFLAGS) $(MINIGBM_CPPFLAGS) $(LIBDRM_CFLAGS)
vkcube_LDADD = $(MINIGBM_LDFLAGS) $(MESA_LDFLAGS) $(LIBDRM_LIBS) -lgbm -lm -lvulkan
