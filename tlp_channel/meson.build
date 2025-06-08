#
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
# Copyright(c) 2025 TLP Channel Test for NVIDIA Firmware
#

project('tlp_channel_test', 'c', version: '0.1.0')

APP_NAME = 'tlp_channel_test'
tlp_channel_test_install_dir = 'bin'

tlp_channel_test_srcs = [
	APP_NAME + '.c',
	'mlx5_ifc.h'
]

tlp_channel_test_deps = [
	dependency('libibverbs', required: true),
	dependency('libmlx5', required : true)
]

tlp_channel_test_link_args = []

tlp_channel_test_c_args = []

executable('tlp_channel_test', tlp_channel_test_srcs,
	dependencies : tlp_channel_test_deps,
	install_dir : tlp_channel_test_install_dir,
	c_args: [tlp_channel_test_c_args],
	link_args:	tlp_channel_test_link_args,
	install: true)

executable('protocol_mode_test', 'protocol_mode_test.c',
	dependencies : tlp_channel_test_deps,
	install_dir : tlp_channel_test_install_dir,
	c_args: [tlp_channel_test_c_args],
	link_args:	tlp_channel_test_link_args,
	install: false) 