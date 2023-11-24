# SPDX-License-Identifier: GPL-2.0

project('abcd',
	'c',
	license : [ 'BSD-3-Clause'],
	meson_version : '>= 0.43.0', # for compiler.get_supported_arguments()
	default_options: [
            'warning_level=2',	# sets -Wextra
            'buildtype=release',
        ])

# Set advanced compiler flags
compiler = meson.get_compiler('c')

compiler_cflags = ['-Wno-unused-parameter',
		   '-Wno-unused-result',
		   '-Wno-missing-field-initializers',
		   '-Wno-sign-compare',
		   '-Wundef',
		   '-Wnull-dereference',
		   '-Wdouble-promotion',
		   '-Wshadow',
		   '-Wpointer-arith',
		   '-Wwrite-strings',
		   '-Wstrict-overflow=4']

# TODO add clang specific options
if compiler.get_id() == 'gcc'
	compiler_cflags += ['-Werror',	# Only set it on GCC
			    '-Wformat-signedness',
			    '-Wduplicated-cond',
			    '-Wduplicated-branches',
			    '-Wvla-larger-than=1',
			    '-Walloc-zero',
			    '-Wunsafe-loop-optimizations',
			    '-Wcast-align',
			    '-Wlogical-op',
			    '-Wjump-misses-init']
endif

add_global_arguments(compiler.get_supported_arguments(compiler_cflags),
		     language: 'c')

client_srcs = ['abcd.c',
	       'circ_buf.c']
executable('abcd',
	   client_srcs,
	   install : true)

ftdi_dep = dependency('libftdi1', required: false)
if not ftdi_dep.found()
	ftdi_dep = dependency('libftdi')
endif

server_deps = [dependency('libudev'),
	       dependency('yaml-0.1'),
	       dependency('libgpiod'),
	       ftdi_dep]
server_srcs = ['abcd-server.c',
	       'circ_buf.c',
	       'conmux.c',
	       'device.c',
	       'device_parser.c',
	       'pyamlboot.c',
	       'dfu.c',
	       'ftdi-gpio.c',
	       'local-gpio.c',
	       'console.c',
	       'ppps.c']
executable('abcd-server',
	   server_srcs,
	   dependencies : server_deps,
	   install : true)
