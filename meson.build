project('vkcube', ['c'], version : '0.1', license : 'MIT',
        default_options : ['c_std=c11'])

cc = meson.get_compiler('c')

dep_m = cc.find_library('m', required : false)

dep_vulkan = dependency('vulkan')
dep_libpng = dependency('libpng')
dep_libdrm = dependency('libdrm')
dep_gbm = dependency('gbm')
dep_wayland_client = dependency('wayland-client', required : get_option('wayland') == 'true')
dep_wayland_protocols = dependency('wayland-protocols', version : '>= 1.12', required : get_option('wayland') == 'true')
dep_xcb = dependency('xcb', required : get_option('xcb') == 'true')

defs = []
if cc.has_header('vulkan/vulkan_intel.h', dependencies: dep_vulkan)
  defs += '-DHAVE_VULKAN_INTEL_H'
endif

if dep_wayland_client.found() and get_option('wayland') != 'false'
  message('wayland enabled')
  defs += '-DENABLE_WAYLAND'

  wayland_scanner = find_program('wayland-scanner')
  wayland_protocols_dir = dep_wayland_protocols.get_pkgconfig_variable('pkgdatadir')

  xdg_shell_xml_path = wayland_protocols_dir + '/stable/xdg-shell/xdg-shell.xml'
  xdg_shell_client_header = custom_target(
        'xdg-shell client-header',
        command: [ wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@' ],
        input: xdg_shell_xml_path,
        output: 'xdg-shell-protocol.h',
        )
    xdg_shell_private_code = custom_target(
        'xdg-shell private-code',
        command: [ wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@' ],
        input: xdg_shell_xml_path,
        output: 'xdg-shell-protocol.c',
        )

  wayland_protocol_files = [ xdg_shell_client_header, xdg_shell_private_code ]
else
  wayland_protocol_files = []
endif

if dep_xcb.found() and get_option('xcb') != 'false'
  message('xcb enabled')
  defs += '-DENABLE_XCB'
endif

# See https://github.com/google/shaderc
prog_glslang = find_program('glslangValidator', required : false)
prog_cp = find_program('cp')

vkcube_files = files(
  'main.c',
  'common.h',
  'cube.c',
  'esTransform.c',
  'esUtil.h'
)

if prog_glslang.found()
  gen = generator(
    prog_glslang,
    output : '@PLAINNAME@.spv.h',
    arguments : [ '@INPUT@', '-V', '-x', '-o', '@OUTPUT@' ]
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
  [vkcube_files, spirv_files, wayland_protocol_files],
  c_args : [ defs, '-Wall',
            '-Werror=implicit-function-declaration',
	    '-Werror=missing-prototypes'],
  dependencies : [dep_libdrm, dep_gbm, dep_libpng, dep_wayland_client, dep_xcb, dep_vulkan, dep_m],
  install : true,
)
