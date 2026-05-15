project(
    'icsim',
    'c', 'cpp',
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
    'can_log.c',
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

# ---- tests ----
test_sources = ['can_bus.c', 'lib.c', 'config.c', 'can_log.c', 'toml.c']
test_deps = socket_deps

test_can_parse = executable('test_can_parse',
    ['tests/test_can_parse.c', test_sources],
    dependencies: test_deps)
test('CAN frame parser', test_can_parse)

test_config = executable('test_config',
    ['tests/test_config.c', test_sources],
    dependencies: test_deps)
test('TOML config loader', test_config)

test_asc = executable('test_asc',
    ['tests/test_asc.c', test_sources],
    dependencies: test_deps)
test('ASC format', test_asc)

# ---- Dear ImGui targets (C++) ----
imgui_sources = [
    'imgui/imgui.cpp',
    'imgui/imgui_draw.cpp',
    'imgui/imgui_widgets.cpp',
    'imgui/imgui_tables.cpp',
    'imgui/backends/imgui_impl_sdl2.cpp',
    'imgui/backends/imgui_impl_sdlrenderer2.cpp',
]
imgui_common = ['can_bus.c', 'can_log.c', 'config.c', 'getopt_compat.c', 'lib.c', 'toml.c']
imgui_deps = deps + socket_deps

imgui_inc = include_directories('imgui', 'imgui/backends')

executable('icsim_imgui',
    ['icsim_imgui.cpp', imgui_common, imgui_sources],
    include_directories: imgui_inc,
    dependencies: imgui_deps,
    override_options: ['warning_level=1'])

executable('controls_imgui',
    ['controls_imgui.cpp', 'can_bus.c', 'config.c', 'toml.c', imgui_sources],
    include_directories: imgui_inc,
    dependencies: imgui_deps,
    override_options: ['warning_level=1'])
