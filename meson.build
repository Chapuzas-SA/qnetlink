project('qnetlink', 'c', version: '0.4.0-dev',
  default_options: ['c_std=c11', 'warning_level=3'])

add_project_arguments('-D_GNU_SOURCE', language: 'c')

slirp_dep = dependency('slirp', version: '>=4.9.3')
glib_dep = dependency('glib-2.0')
lws_dep = dependency('libwebsockets', version: '>=5.0', static: true)
cc = meson.get_compiler('c')
deps = [slirp_dep, glib_dep, lws_dep]
if host_machine.system() == 'windows'
  ws2_dep = cc.find_library('ws2_32', required: true)
  deps += ws2_dep
else
  deps += dependency('openssl')
endif
platform_source = host_machine.system() == 'windows' ? 'src/net/platform_win32.c' : 'src/net/platform_posix.c'
src_inc = include_directories('.', 'src')

conf = configuration_data()
conf.set_quoted('QNETLINK_VERSION', meson.project_version())
build_timestamp = run_command('date', '-u', '+%Y%m%dT%H%M%S%6NZ', check: true).stdout().strip()
conf.set_quoted('QNETLINK_BUILD_ID', meson.project_version() + '+' + build_timestamp)
configure_file(output: 'version.h', configuration: conf)

executable('qnetlink',
  ['src/app/main.c', 'src/app/config.c', 'src/app/logger.c', 'src/protocol/stream.c', 'src/app/slirp_backend.c', 'src/net/transport_tcp.c', 'src/net/transport_wss.c', 'src/protocol/wss_message.c', 'src/net/wss_server.c', 'src/app/event_loop.c', platform_source],
  include_directories: src_inc,
  dependencies: deps,
  install: true,
  install_rpath: host_machine.system() == 'windows' ? '' : '$ORIGIN/lib')
