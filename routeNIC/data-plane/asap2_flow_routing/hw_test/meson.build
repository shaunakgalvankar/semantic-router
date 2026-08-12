project('ROUTENIC_ASAP2_TEST', 'C',
	version: '0.1',
	license: 'BSD-3',
	default_options: ['buildtype=debug'],
	meson_version: '>= 0.61.2'
)

add_project_arguments('-D DOCA_ALLOW_EXPERIMENTAL_API', language: ['c'])

sample_dependencies = []
sample_dependencies += dependency('doca-common')
sample_dependencies += dependency('doca-flow')
sample_dependencies += dependency('doca-dpdk-bridge')
sample_dependencies += dependency('doca-argp')
sample_dependencies += dependency('libdpdk')

c_compiler = meson.get_compiler('c')
if c_compiler.has_header_symbol('rte_pmd_mlx5.h', 'rte_pmd_mlx5_driver_disable_steering',
				dependencies: dependency('libdpdk'), prefix: '#include <rte_pmd_mlx5.h>')
	add_project_arguments('-D DOCA_BUNDLE_DPDK_FOUND', language: ['c'])
elif c_compiler.has_header_symbol('rte_pmd_mlx5.h', 'rte_pmd_mlx5_disable_steering',
				dependencies: dependency('libdpdk'), prefix: '#include <rte_pmd_mlx5.h>')
	add_project_arguments('-D UPSTREAM_DPDK_FOUND', language: ['c'])
endif

# DOCA install location — override with `meson setup build -Ddoca_prefix=...`
# on a host where DOCA lives somewhere else.
doca_prefix = get_option('doca_prefix')
doca_samples = doca_prefix + '/samples'
doca_apps_common = doca_prefix + '/applications/common'

sample_srcs = [
	'asap2_test_main.c',
	'asap2_test_sample.c',
	'../asap2_tenant_steering.c',
	doca_samples + '/common.c',
	doca_samples + '/doca_flow/flow_common.c',
	doca_apps_common + '/dpdk_utils.c',
]

# Absolute -I flags (not meson include_directories(), which rejects/mangles
# paths outside the source tree) so this build can reuse the real DOCA
# sample-common sources in place, without copying them.
add_project_arguments(
	'-I' + doca_samples,
	'-I' + doca_samples + '/doca_flow',
	'-I' + doca_apps_common,
	language: ['c'],
)

executable('routenic_asap2_test', sample_srcs,
	c_args: '-Wno-missing-braces',
	dependencies: sample_dependencies,
	include_directories: include_directories('.'),
	install: false)
