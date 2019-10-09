project('vkcube', ['c'], version : '0.1', license : 'MIT',
        default_options : ['c_std=c11'])

cc = meson.get_compiler('c')

dep_libdrm = dependency('libdrm')
dep_gbm = dependency('gbm')
dep_libpng = dependency('libpng')
dep_wayland = dependency('wayland-client')
dep_xcb = dependency('xcb')
dep_vulkan = dependency('vulkan')
dep_m = cc.find_library('m', required : false)

cc = meson.get_compiler('c')
defs = []
if cc.has_header('vulkan/vulkan_intel.h')
  defs += '-DHAVE_VULKAN_INTEL_H'
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
    output : '@PLAINNAME@',
    arguments : [ '@INPUT@', '-mfmt=num', '-o', '@OUTPUT@.spv.h' ]
  )
else
  gen = generator(
    prog_cp,
    output : '@PLAINNAME@',
    arguments : [ '@INPUT@.spv.h.in', '@OUTPUT@.spv.h' ]
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
