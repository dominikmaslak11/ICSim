Instrument Cluster Simulator for SocketCAN
==========================================

By: OpenGarages <agent.craig@gmail.com>

Quick Start (Windows)
---------------------

For the modern Dear ImGui UI, run the full local test stack from PowerShell:

```
  .\run_windows_savvycan.ps1
```

This starts:

```
  builddir\icsim_imgui.exe vcan0
  builddir\icsim_imgui.exe --controls vcan0
  builddir\savvycan_bridge.exe --stats vcan0
  SavvyCAN.exe
```

The controls window sends dashboard frames and background CAN traffic by
default. Use the **BG CAN** checkbox in the controls window to pause or resume
background traffic while the simulator is running. To start without background
traffic:

```
  .\run_windows_savvycan.ps1 -NoNoise
```

You can still run the legacy SDL windows manually. Open **three** terminals
(cmd.exe, PowerShell, or MSYS2) in the `builddir\` directory, then run:

```
Terminal 1:  icsim.exe vcan0
Terminal 2:  controls.exe -X vcan0
Terminal 3:  savvycan_bridge.exe vcan0
```

Now open SavvyCAN, go to **Connection → Open → Add New Connection**:

1. Type: **Network Connection (GVRET)**
2. Host: **127.0.0.1**
3. Port: **23**
4. Bus: **0**
5. Bit rate: **500000**

Click **Connect**. You will see CAN frames flowing from the simulator in
SavvyCAN's frame view.

> **Important:** All programs must run from the `builddir\` directory because
> they depend on DLL files (SDL2.dll, SDL2_image.dll, and others) located there.
> All three must use the **same bus name** (default: `vcan0`).

Quick Start (Linux)
-------------------

```
  sudo modprobe can vcan
  sudo ip link add dev vcan0 type vcan
  sudo ip link set up vcan0

  ./icsim vcan0         # Terminal 1
  ./controls vcan0      # Terminal 2
```

Compiling
---------

The project uses the [Meson build system](https://mesonbuild.com/).

### Linux

```
  sudo apt-get install libsdl2-dev libsdl2-image-dev can-utils
  meson setup builddir && cd builddir
  meson compile
```

### Windows

Install dependencies with **MSYS2 MinGW 64-bit**:

```
  pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-meson \
            mingw-w64-x86_64-SDL2 mingw-w64-x86_64-SDL2_image
```

Build from an **MSYS2 MinGW 64-bit** shell:

```
  meson setup builddir
  meson compile -C builddir
```

On Windows, SocketCAN kernel drivers are not available. This project provides
a built-in virtual CAN bus using **local UDP multicast** — no kernel driver
or admin setup is required. All programs that share the same bus name
(e.g. `vcan0`) automatically exchange CAN frames with each other.

SavvyCAN Integration (Windows)
------------------------------

The `savvycan_bridge.exe` program bridges the internal virtual CAN bus to
SavvyCAN via the GVRET binary protocol over TCP.

### Setup

1. Start the full ImGui stack with `.\run_windows_savvycan.ps1`.
3. In SavvyCAN, add a **Network Connection (GVRET)**:
   - Host: `127.0.0.1`
   - Port: `23`
   - Bus: `0`
   - Bit rate: `500000`
4. Frames appear immediately in SavvyCAN's frame view. You should see both
   simulator control frames and background CAN traffic unless `-NoNoise` was
   used or **BG CAN** is unchecked.

The bridge exposes **one CAN bus at 500 kbit/s** on the standard GVRET
TCP port (23). Frames sent from SavvyCAN are injected back into the virtual
bus — you can use SavvyCAN to send frames that `icsim` will process.

> **Firewall:** The first time you run the bridge, Windows Firewall may
> prompt you to allow network access. Click **Allow**.

### WebSocket Bridge (browser dashboard)

A second bridge is available that serves a live CAN dashboard in any browser:

```
  builddir\websocket_bridge.exe vcan0
```

Open `http://127.0.0.1:8080` — you'll see a real-time table of CAN frames
and a form to inject frames back into the bus. Works alongside the GVRET
bridge on port 23.

### Recording and replay (ASC format)

Record all CAN traffic to a Vector ASC file:

```
  icsim.exe -R session.asc vcan0
```

The ASC file can be opened in Wireshark, SavvyCAN, or CANalyzer.

Replay a recorded session with original timing:

```
  icsim.exe -P session.asc vcan0
```

Recording and replay can be combined — record while replaying captures
the full bus traffic including injected frames.

### Testing the bus manually

You can inject frames manually without starting the GUI:

```
  builddir\cansend.exe vcan0 19B#000001
  builddir\candump.exe vcan0
```

Usage
-----

### Default mode

```
  ./icsim vcan0       # Instrument Cluster window
  ./controls vcan0    # Control panel window
```

The hard-coded defaults are in sync — the controls app generates CAN packets
based on your inputs (keyboard or gamepad), and the IC simulator sniffs the
bus and updates the dashboard display.

Default CAN IDs:
- Speed:  `0x244`
- Doors:  `0x19B`
- Signals: `0x188`
- RPM:    `0x0AA`
- Coolant: `0x1B8`
- Fuel:   `0x2C8`

### New signal controls

| Key | Action |
|-----|--------|
| `1` / `2` | RPM ±500 |
| `3` / `4` | Coolant temp ±5°C |
| `5` / `6` | Fuel level ±5% |
| `↑` | Accelerate |
| `←` / `→` | Turn signals |

The IC simulator displays RPM, coolant temp, and fuel as colored bar
indicators below the speedometer. Temp turns red above 105°C, fuel turns
red below 15%.

### Controls keyboard mapping

| Key | Action |
|-----|--------|
| `↑` | Accelerate |
| `←` / `→` | Turn signals |
| `LShift + A/B/X/Y` | Lock individual doors |
| `RShift + A/B/X/Y` | Unlock individual doors |
| `A/B/X/Y` (no shift) | Toggle individual doors |

### Windows-specific options

The `-X` flag disables background CAN traffic, which is **required on Windows**
since `canplayer` is not available:

```
  controls.exe -X vcan0
```

On Windows the background traffic thread reads `data/sample-can.log` and
replays it over the virtual bus instead of forking `canplayer`.

Troubleshooting
---------------

### "System cannot find DLL" (Windows)

Run the programs from the `builddir\` directory — all required DLLs
(SDL2.dll, SDL2_image.dll, libwinpthread-1.dll, etc.) are located there.

### Port 23 already in use (Windows)

The GVRET bridge binds to TCP port 23. If another program is using it,
the bridge will fail to start. Stop the conflicting program or use an
alternative SavvyCAN connection method.

### SavvyCAN shows no frames

Verify the bridge is listening:
```
  netstat -ano | findstr ":23"
```

Make sure `icsim.exe` and `controls.exe` are running with the **same bus name**
as `savvycan_bridge.exe` (default: `vcan0`). If the cluster shows no movement,
try pressing keys in the controls window or send a test frame:
```
  cansend.exe vcan0 19B#000001
```

### read: Bad Address (Linux)

Recompile with updated SDL libraries. Make sure you have the latest SDL2.
Some users have fixed this by creating symlinks to SDL.h or editing the
Makefile CFLAGS to point to the correct SDL2 include directory, e.g.
`/usr/include/x86_64-linux-gnu/SDL2`. Arch Linux may also need `sdl2_gfx`.

### lib.o not linking

This project now uses **Meson** which compiles `lib.c` directly from source.
The pre-compiled `lib.o` has been removed. If you were using the old Makefile,
switch to Meson instead (`meson setup builddir && meson compile -C builddir`).

### canplayer errors (Linux)

Install can-utils: `sudo apt-get install can-utils`

Testing on a virtual CAN interface
----------------------------------

```
  sudo modprobe can vcan
  sudo ip link add dev vcan0 type vcan
  sudo ip link set up vcan0
```

Use `ifconfig vcan0` to verify the interface. A `setup_vcan.sh` script is
also provided.

On Windows, skip this step — the virtual bus is built-in and requires no
kernel setup.

CAN Hacking Training Usage
--------------------------

To *safely* train on CAN hacking you can play back a sample recording of
generic CAN traffic (included in `data/sample-can.log`). This creates
something similar to normal CAN "noise". Then start the IC Sim with the
`-r` (randomize) switch:

```
  ./icsim -r vcan0
  Using CAN interface vcan0
  Seed: 1401717026
```

Now copy the seed number and pass it as the `-s` (seed) option for the controls:

```
  ./controls -s 1401717026 vcan0
```

This randomizes which CAN IDs and byte positions the IC simulator expects.
Passing the same seed to the controls keeps them in sync. Use SavvyCAN or
`candump` to hunt down which packets affect the dashboard.

For the most realistic training, increase the difficulty:

```
  ./controls -s 1401717026 -l 2 vcan0
```

Difficulty levels:
- **0** — Only ID and byte position are randomized
- **1** — Adds NULL padding to unused bytes
- **2** — Fills unused bytes with random data (simulates multi-signal CAN frames)

On Windows, add the `-X` flag:

```
  controls.exe -X -s 1401717026 -l 2 vcan0
```

To observe the bus during training, add the SavvyCAN bridge:

```
  savvycan_bridge.exe vcan0
```

Then connect SavvyCAN to `127.0.0.1:23`.

Pre-recorded scenarios are available in `scenarios/`:

```
  icsim.exe -P scenarios/cold_start.asc vcan0
  icsim.exe -P scenarios/highway.asc vcan0
  icsim.exe -P scenarios/emergency.asc vcan0
```

Replay a scenario and watch the dashboard react — cold start warmup,
highway lane changes with turn signals, or emergency hard brake with
hazard lights. Use SavvyCAN or the WebSocket dashboard to inspect the
CAN traffic during playback.

### Headless mode

Run ICSim without a GUI — ideal for servers, CI/CD, and Docker:

```
  icsim.exe --headless vcan0
  icsim.exe --headless --duration 30 vcan0   # run for 30 seconds
```

State changes (speed, RPM, doors, signals, temperature, fuel) are
printed to stdout. Combine with recording for automated testing:

```
  icsim.exe --headless -P scenarios/emergency.asc -R output.asc vcan0
```

### Docker

Build and run the full stack:

```
  docker build -t icsim .
  docker run --rm -p 23:23 -p 8080:8080 icsim
```

This starts ICSim (headless), the GVRET bridge, and the WebSocket
dashboard all in one container. SavvyCAN connects to port 23,
browser to `http://localhost:8080`.

Or use docker compose:

```
  docker compose up
```
