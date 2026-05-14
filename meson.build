project(
    'icsim',
    'c',
    license: 'GPL-3',
    default_options: [
        'warning_level=2',
    ]
)

deps = [
    dependency('sdl2', required: true),
    dependency('SDL2_image', required: true)
]

cc = meson.get_compiler('c')
socket_deps = []
if host_machine.system() == 'windows'
    socket_deps += cc.find_library('ws2_32', required: true)
endif

common_sources = [
    'can_bus.c',
    'getopt_compat.c',
    'lib.c',
]

subdir('art')
subdir('data')

executable('icsim', ['icsim.c', common_sources], dependencies: deps + socket_deps)
executable('controls', ['controls.c', common_sources], dependencies: deps + socket_deps)
