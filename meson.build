project('vkcube', ['c'], version : '0.1', license : 'MIT',
        default_options : ['c_std=c11'])

cc = meson.get_compiler('c')

dep_libdrm = dependency('libdrm')
dep_gbm = dependency('gbm')
dep_libpng = dependency('libpng')
dep_wayland = dependency('wayland-client', required : get_option('wayland'))
dep_xcb = dependency('xcb', required : get_option('x11'))
dep_vulkan = dependency('vulkan')
dep_m = cc.find_library('m', required : false)

cc = meson.get_compiler('c')
defs = []
if cc.has_header('vulkan/vulkan_intel.h')
  defs += '-DHAVE_VULKAN_INTEL_H'
endif

if get_option('wayland').enabled()
  defs += '-DVKCUBE_USE_WAYLAND'
endif

if get_option('x11').enabled()
  defs += '-DVKCUBE_USE_X11'
endif

# See https://github.com/google/shaderc
prog_glslc = find_program('glslc', required : false)
prog_cp = find_program('cp')

vkcube_files = files(
  'xdg-shell-protocol.c',
  'main.c',
  'common.h',
  'cube.c',
  'esTransform.c',
  'esUtil.h'
)

if prog_glslc.found()
  gen = generator(
    prog_glslc,
    output : '@PLAINNAME@.spv.h',
    arguments : [ '@INPUT@', '-mfmt=num', '-o', '@OUTPUT@' ]
  )
else
  gen = generator(
    prog_cp,
    output : '@PLAINNAME@.spv.h',
    arguments : [ '@INPUT@.spv.h.in', '@OUTPUT@' ]
  )
endif

spirv_files = gen.process('vkcube.vert', 'vkcube.frag')
vkcube = executable(
  'vkcube',
  [vkcube_files, spirv_files],
  c_args : [ defs, '-Wall',
            '-Werror=implicit-function-declaration',
	    '-Werror=missing-prototypes'],
  dependencies : [dep_libdrm, dep_gbm, dep_libpng, dep_wayland, dep_xcb, dep_vulkan, dep_m],
)
