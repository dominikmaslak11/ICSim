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
    'config.c',
    'getopt_compat.c',
    'lib.c',
    'toml.c',
]

subdir('art')
subdir('data')

executable('icsim', ['icsim.c', common_sources], dependencies: deps + socket_deps)
executable('controls', ['controls.c', common_sources], dependencies: deps + socket_deps)
if host_machine.system() == 'windows'
    executable('savvycan_bridge', ['savvycan_bridge.c', 'can_bus.c'], dependencies: socket_deps)
endif
executable('websocket_bridge',
    ['websocket_bridge.c', 'can_bus.c', 'mongoose.c'],
    dependencies: socket_deps,
    c_args: ['-DMG_ENABLE_SOCKET=1', '-DMG_ENABLE_CUSTOM_MILLIS=0'])
executable('cansend', ['cansend.c', 'can_bus.c', 'lib.c'], dependencies: socket_deps)
executable('candump', ['candump.c', 'can_bus.c', 'lib.c'], dependencies: socket_deps)
