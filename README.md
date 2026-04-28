# OSC client/server examples — Python + cross-platform C++

Minimal, dependency-free OSC 1.0 over UDP. The Python and C++ implementations
are wire-compatible: any client can talk to any server.

## Layout

```
.
├── README.md
├── python/
│   ├── osc_common.py   # pure-stdlib OSC encoder/decoder
│   ├── theremin.py     # sine-wave synth (sounddevice if installed, silent otherwise)
│   ├── osc_server.py   # Tkinter UI: echo/pause toggles, mirrored state, 5 push sliders
│   └── osc_client.py   # Tkinter UI: buttons, sliders, theremin (pitch/vol/on-off), log
└── cpp/
    ├── osc.hpp         # header-only OSC + cross-platform sockets
    ├── theremin.hpp    # sine-wave synth (ALSA/CoreAudio/WinMM, silent fallback)
    ├── osc_server.cpp  # ANSI-terminal dashboard with stdin commands and 5 push sliders
    ├── osc_client.cpp  # ANSI-terminal dashboard with theremin commands
    └── Makefile        # builds osc_server and osc_client; auto-detects audio libs
```

## Digital theremin (real audio on the client)

Each client carries a tiny digital theremin — a continuous sine wave whose
**pitch** and **volume** can be set live, gated by an **on/off** control.
Both the client (locally) and the server (remotely) can drive it. The client
has the on/off switch; the server can only adjust pitch and volume.

OSC addresses on the wire:

| direction         | address                       | type     | meaning                       |
|-------------------|-------------------------------|----------|-------------------------------|
| client → server   | `/theremin/pitch`             | float Hz | client moved its pitch slider |
| client → server   | `/theremin/volume`            | float    | client moved its volume slider|
| client → server   | `/theremin/on`                | bool     | client toggled on/off         |
| server → client   | `/server/theremin/pitch`      | float Hz | server pushed pitch           |
| server → client   | `/server/theremin/volume`     | float    | server pushed volume          |

Audio backends, picked at compile/install time:

| client        | backend                               | how to enable                            |
|---------------|----------------------------------------|------------------------------------------|
| Python client | `sounddevice` + `numpy`               | `pip install -r python/requirements.txt` |
| C++ Linux     | ALSA                                  | `sudo apt install libasound2-dev` then `make` |
| C++ macOS     | CoreAudio (AudioToolbox)              | works out of the box                     |
| C++ Windows   | WinMM                                  | works out of the box                     |
| any platform  | silent (no sound, UI still works)     | automatic fallback if libs missing       |

> **No sound coming out?** That almost always means the audio library isn't
> installed. The Python client shows a red **NO SOUND** banner in the
> Theremin frame with the exact pip command to fix it; click **Test Sound
> (1s)** after installing to verify a 440 Hz tone plays.

The Makefile compiles a tiny ALSA test on Linux to decide whether to link
`-lasound`; if either the header or the library is missing, it falls back
to the silent backend so `make` always succeeds.

## Renaming sliders

The Frequency, Gain, and LFO labels are editable on the server, and the
new name propagates to the client.

| program       | how to rename                                              |
|---------------|------------------------------------------------------------|
| Python server | type into the label text box on the slider row, hit Enter  |
| C++ server    | `L <slot> <new name>` (or `rename ...`), where slot is 1=Frequency, 2=Gain, 3=LFO |

OSC addresses on the wire (server → client only):

| address                  | type   | meaning                          |
|--------------------------|--------|----------------------------------|
| `/server/rename/freq`    | string | new name for the Frequency row  |
| `/server/rename/gain`    | string | new name for the Gain row       |
| `/server/rename/lfo`     | string | new name for the LFO row        |

The OSC addresses for the actual values (`/server/freq`, `/synth/freq`,
etc.) don't change — only the user-visible label is renamed.

## Editable host/port

All four UIs let you change the address at runtime — the command-line `--host` /
positional `host port` arguments are just initial defaults.

| Program       | Where                                  | How                              |
|---------------|----------------------------------------|----------------------------------|
| Python server | "Bind address" frame at the top        | edit Host/Port + click **Rebind**|
| Python client | "Target address" frame at the top      | edit Host/Port + click **Apply** |
| C++ server    | type at the dashboard, press Enter     | `r <host> <port>` (rebinds)      |
| C++ client    | type at the dashboard, press Enter     | `t <host> <port>` (retargets)    |

Rebind on the server closes the old socket cleanly; if the new bind fails
(e.g., port in use) the dashboard shows `(unbound)` and you can try again.

## What the UIs show

All four programs share a UI. The clients drive the server, the server can
drive the clients back, and every UI mirrors the state it sees on the wire.
Clients bind an ephemeral UDP port so the server's `/ack...` replies reach
them; the server tracks the most recent sender so its push controls have a
target.

**Python client (Tkinter):** Play / Stop / `/hello` buttons, sliders for
frequency (20–2000 Hz), gain (0–1), and LFO (0–1) sending on every change,
plus a note+velocity trigger. The log shows sent messages in green (`->`)
and received `/ack` replies in blue (`<-`).

**C++ client (terminal dashboard):** auto-runs a demo while displaying live
progress bars for `freq`, `gain`, and `lfo`, the current transport state, and
a scrolling log. Uses ANSI escape codes; on Windows 10+ ANSI is enabled
automatically via `SetConsoleMode`.

**Python server (Tkinter):** Echo and Pause checkboxes; three push sliders
(`/server/freq`, `/server/gain`, `/server/lfo`) whose labels are editable and
synced to the client; theremin push sliders with the client's on/off state
shown read-only; four action buttons (Ping, Announce, Tell client: Play /
Stop); and a colored log.

**C++ server (terminal dashboard):** address bar, stats, push sliders, and
theremin row with the client's on/off state mirrored. Drive it with
single-letter commands typed at the terminal followed by Enter:

```
e         toggle echo (reply with /ack<address>)
p         toggle pause (drop incoming)
c         clear log
1 / 2 / 3 select push slider (freq / gain / lfo)
+ / -     nudge selected slider (auto-pushes)
! / @ / # push current freq / gain / lfo value to last sender
b         /server/ping  to last sender
a         /server/announce "hello from server"
y / n     /server/transport/play  /  /server/transport/stop
h         help in log
q         quit
```

## Supported OSC types

`i` int32, `f` float32, `s` string, `b` blob, `T`/`F` bool, `N` nil, `I` infinitum.
Bundles and timetags are intentionally not implemented — keep it simple.

## Run it

### Python only
```sh
cd python
python3 osc_server.py            # terminal 1
python3 osc_client.py            # terminal 2
```

### C++ only
```sh
cd cpp
make                             # builds osc_server and osc_client
./osc_server                     # terminal 1
./osc_client                     # terminal 2
```

### Mix and match (proves wire compatibility)
```sh
cpp/osc_server &                          # C++ server
python3 python/osc_client.py              # Python client → C++ server

python3 python/osc_server.py &            # Python server
cpp/osc_client                            # C++ client → Python server
```

Default address is `127.0.0.1:9000`. Override on the command line:
```sh
python3 python/osc_server.py --host 0.0.0.0 --port 9001
cpp/osc_client 192.168.1.20 9001
```

## Building C++ on each platform

Run from inside the `cpp/` folder:

| Platform        | Command                                                              |
|-----------------|----------------------------------------------------------------------|
| Linux / macOS   | `c++ -std=c++17 -O2 osc_server.cpp -o osc_server`                    |
| Windows MSVC    | `cl /std:c++17 /EHsc osc_server.cpp /Fe:osc_server.exe`              |
| Windows MinGW   | `g++ -std=c++17 -O2 osc_server.cpp -o osc_server.exe -lws2_32`       |

`osc.hpp` selects between Winsock2 and BSD sockets via `#ifdef _WIN32` and
calls `WSAStartup` automatically on first use.

## Sample output

C++ server receiving from the Python client:
```
[from 127.0.0.1:53894] /hello             typetag=",s"   args=["world"]
[from 127.0.0.1:53894] /synth/freq        typetag=",f"   args=[440f]
[from 127.0.0.1:53894] /synth/note        typetag=",ii"  args=[60i, 100i]
[from 127.0.0.1:53894] /mixer/channel/1/gain typetag=",f" args=[0.75f]
[from 127.0.0.1:53894] /transport/play    typetag=",T"   args=[true]
```

Python server receiving from the C++ client:
```
[from 127.0.0.1:53455] /hello              args=['world']
[from 127.0.0.1:53455] /synth/freq         args=[440.0]
[from 127.0.0.1:53455] /synth/note         args=[60, 100]
[from 127.0.0.1:53455] /mixer/channel/1/gain args=[0.75]
[from 127.0.0.1:53455] /transport/play     args=[True]
```

## Notes

The OSC 1.0 wire format used here:

* Each message is one UDP datagram.
* Strings: UTF-8 bytes, NUL-terminated, zero-padded to a 4-byte boundary.
* Blobs: int32 size prefix, raw bytes, zero-padded to a 4-byte boundary.
* Numerics: 32-bit big-endian (network byte order). Floats use IEEE-754.
* Type tag string starts with `,` followed by one character per argument.
