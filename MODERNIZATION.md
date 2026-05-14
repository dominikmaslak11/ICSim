# ICSim Modernization Plan

> Status: In Progress — Phase 1 (TOML config + code cleanup)
> Last updated: 2026-05-14

---

## Phase 1: TOML Configuration System (current)

Goal: replace hardcoded `#define` CAN IDs, byte positions, and dashboard layouts
with a runtime-loaded TOML file per vehicle model. New models = new file, zero
recompilation.

### 1.1 TOML config format

```toml
# models/default.toml
[vehicle]
name = "Default"
description = "Stock ICSim vehicle with speed, doors, and turn signals"

[can]
speed_id = "0x244"
speed_byte = 3
door_id = "0x19B"
door_byte = 2
signal_id = "0x188"
signal_byte = 0

[signals.speed]
length = 2             # bytes in CAN frame
scaling = 0.6213751    # raw to mph multiplier
divisor = 100

[signals.doors]
length = 1
door1_mask = 0x01
door2_mask = 0x02
door3_mask = 0x04
door4_mask = 0x08

[signals.turn]
length = 1
left_mask = 0x01
right_mask = 0x02

[dashboard]
width = 692
height = 329

[dashboard.speedometer]
x = 200
y = 80
width = 300
height = 130
needle_center_x = 135
needle_center_y = 20
angle_min = 0
angle_max = 180
value_min = 0
value_max = 280
```

### 1.2 Implementation

- Uses [tomlc99](https://github.com/cktan/tomlc99) (MIT, single .c/.h, C99)
- New files: `config.c`, `config.h`, `toml.c`, `toml.h`
- Loaded via `-m models/default.toml` (or `-m bmw_x1` resolves to `models/bmw_x1.toml`)
- Falls back to built-in defaults if no config

### 1.3 Migration

- Phase 1 keeps hardcoded defaults as fallback
- Phase 2 (future): move BMW X1 to `models/bmw_x1.toml`
- Phase 3 (future): remove hardcoded fallback entirely

---

## Phase 2: Code Cleanup

### 2.1 Remove easter egg
- `kk_check()`, `kkpay()` in `controls.c` - dead code, no effect on functionality

### 2.2 Refactor main loops
- Extract SDL event loop from `icsim.c` main() (~500 lines to smaller functions)
- Extract CAN frame processing into `can_dispatch.c`

### 2.3 Build system
- Remove `lib.o` binary blob - build from source
- Consider removing Makefile (meson is now primary)
- Add `clang-format` config

---

## Phase 3: Dear ImGui Interface

Replace manual SDL sprite rendering with ImGui widgets.

### Goals
- Dashboard rendered as ImGui gauge/progress/icon widgets
- Controls panel as ImGui sliders, checkboxes
- Debug overlay showing raw CAN frames in real time
- Hot-reloadable skins

### Impact
- ~200 lines of rendering code to ~50 lines of widget declarations
- Backends: SDL2 + OpenGL (already available on Windows)
- ImGui is single-header MIT - no new dependencies to install

---

## Phase 4: WebSocket Bridge

Browser-accessible dashboard and CAN inspector.

### Architecture
- `websocket_bridge.c` - serves dashboard HTML + WebSocket CAN stream
- Port 8080 alongside existing GVRET (port 23)
- REST API: `GET /api/frames`, `POST /api/send`, `GET /api/status`

### Tech
- [mongoose](https://github.com/cesanta/mongoose) (single .c/.h, MIT/GPLv2)
  or [libwebsockets](https://libwebsockets.org/)

### Use case
Training sessions - every student opens `http://server:8080` in a browser.
No software installation required.

---

## Phase 5: Extended Signals + Scenarios

### New CAN signals
- Engine RPM (`0x0AA` - already defined for BMW mode)
- Coolant temperature
- Fuel level
- Check engine light
- ABS warning
- Airbag status
- Headlights / high beam
- Windshield wipers
- Odometer

### CAN FD
- Use `CANFD_MAX_DLEN=64` for single-frame full-state broadcasts
- Useful for headless mode state sync

### Predefined scenarios
- `cold_start.toml` - engine cranking, idle warmup
- `highway.toml` - steady cruise, lane changes
- `emergency.toml` - hard brake, ABS activation, hazard lights
- `night_drive.toml` - headlights, instrument dimming

---

## Phase 6: Headless Mode + Docker

### Headless mode
- `--headless` flag: skip SDL initialization, run as pure CAN node
- Consumes CAN frames (updates internal state), responds if configured
- STDOUT logging of state changes

### Docker image
```dockerfile
FROM debian:bookworm-slim
COPY builddir/icsim /usr/local/bin/
EXPOSE 23 8080
CMD ["icsim", "--headless", "vcan0"]
```
Used for CI/CD testing and remote training servers.

---

## Phase 7: CAN Recording + Replay

### Features
- `--record session.asc` - Vector ASC format, compatible with CANalyzer
- `--replay session.asc` - playback with original timing
- `--snapshot state.bin` - save/restore full dashboard state
- Support for PCAP-NG (Wireshark-compatible) via `candump` format

---

## Phase 8: Tests + CI

### Unit tests
- CAN frame parser (`parse_canframe` in lib.c)
- Config loader (TOML to internal struct)
- GVRET binary protocol parser (regression test for the off-by-one fix)

### Framework
- [criterion](https://github.com/Snaipe/Criterion) (C, xUnit-style, MIT)
  or [cmocka](https://cmocka.org/) (C, mock support, Apache 2.0)

### CI pipeline
- GitHub Actions: build on Linux (gcc + clang) and Windows (MSYS2 mingw)
- Run tests, check formatting, build artifacts

---

## Prioritized Execution Order

| Phase | Effort | Impact | Risk | Depends on |
|-------|--------|--------|------|------------|
| 1 - TOML config | 2-3h | High | Low | nothing |
| 2 - Code cleanup | 1-2h | Medium | Low | nothing |
| 3 - ImGui | 4-6h | High | Medium | Phase 2 |
| 4 - WebSocket | 2-3h | Medium | Low | nothing |
| 5 - Signals + scenarios | 3-4h | High | Low | Phase 1 |
| 6 - Docker + headless | 1-2h | Medium | Low | Phase 5 |
| 7 - Recording/replay | 2-3h | Medium | Low | nothing |
| 8 - Tests + CI | 3-4h | Medium | Low | Phase 2 |

Phases 1, 2, 4, 7 can run in parallel. Phase 3 depends on Phase 2 cleanup.
Phase 5 depends on Phase 1 (config system). Phase 6 depends on Phase 5.

---

## Notes

- All new dependencies are single-header C99 libraries (MIT license) - no
  package manager needed, no build system changes beyond adding .c files.
- Backward compatibility: all existing CLI flags and behavior preserved.
  Config files are additive - fall back to built-in defaults when absent.
- Windows-first: phases tested and working on MSYS2 MinGW-w64 before Linux.
