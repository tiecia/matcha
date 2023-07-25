project('matcha', 'c',
  version : '1.0.1',
  default_options : ['warning_level=3','werror=true', 'c_std=gnu17'])

wayland_client = dependency('wayland-client', required: true)

matcha_srcs = files('src/main.c')

# Generate Wayland Protocols stuff
wl_protocols_dep = dependency('wayland-protocols')
wl_protocols_dir = wl_protocols_dep.get_variable('pkgdatadir')

wl_scanner_dep = dependency('wayland-scanner', native: true)
wl_scanner_prog = find_program(wl_scanner_dep.get_variable('wayland_scanner'), native: true)

wl_inhibit_xml = wl_protocols_dir / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml'

wl_inhibit_inc = custom_target(
  '@BASENAME@.h',
  output: '@BASENAME@.h',
  input: wl_inhibit_xml,
  command: [wl_scanner_prog, 'client-header', '@INPUT@', '@OUTPUT@']
)


wl_inhibit_src = custom_target(
  '@BASENAME@.c',
  output: '@BASENAME@.c',
  input: wl_inhibit_xml,
  command: [wl_scanner_prog, 'private-code', '@INPUT@', '@OUTPUT@']
)

wl_inhibit_dep = declare_dependency(
  sources: [wl_inhibit_inc, wl_inhibit_src],
  dependencies: [wayland_client],
)


matcha_exe = executable('matcha',
  matcha_srcs,
  dependencies: wl_inhibit_dep
)
