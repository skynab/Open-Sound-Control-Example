"""
osc_server.py - OSC server with a small Tkinter UI.

* Listens on UDP --host:--port (default 127.0.0.1:9000).
* Mirrors state from common addresses (/synth/freq, /mixer/channel/1/gain,
  /lfo/value, /transport/play|stop) into on-screen labels.
* Echo toggle: when on, replies to every message with /ack<address> + the
  same args (so UI clients can show round-trip data).
* Pause toggle: when on, incoming packets are dropped entirely.
* Three push sliders + buttons that send messages back to the last seen
  client (/server/freq, /server/gain, /server/lfo, /server/ping,
  /server/announce, /server/transport/play|stop). This lets the server
  drive the client.

Tkinter ships with the Python stdlib; no extra deps. Just:

    python3 osc_server.py
    python3 osc_server.py --host 0.0.0.0 --port 9000
"""

import argparse
import queue
import socket
import threading
import tkinter as tk
from tkinter import scrolledtext

from osc_common import decode_message, encode_message


class OscServer:
    def __init__(self, root: tk.Tk, host: str, port: int) -> None:
        self.root = root
        self.host = host
        self.port = port

        self.sock = None  # set by _bind()
        self.recv_thread: "threading.Thread | None" = None

        # Inbox from the recv thread (Tk is single-threaded; we only touch
        # widgets from the main thread, polling this queue).
        self.q: "queue.Queue[tuple]" = queue.Queue()
        self.running = True
        self.last_sender = None  # (host, port) of the most recent client
        self.total = 0

        # Tk variables used by the UI
        self.echo_var      = tk.BooleanVar(value=True)
        self.paused_var    = tk.BooleanVar(value=False)
        self.last_sender_v = tk.StringVar(value="(none)")
        self.total_v       = tk.StringVar(value="0")
        # Theremin on/off is mirrored from the client (the only piece of
        # incoming state we still surface, since it's displayed in the
        # Theremin push frame next to the pitch/volume sliders).
        self.therm_on_v    = tk.StringVar(value="-")

        # Editable display names for the three core sliders. Renaming any
        # of these on the server pushes a /server/rename/<slot> message
        # to the last-seen client so it relabels its matching slider too.
        self.name_freq_v = tk.StringVar(value="Frequency")
        self.name_gain_v = tk.StringVar(value="Gain")
        self.name_lfo_v  = tk.StringVar(value="LFO")
        self._last_sent_names: dict[str, str] = {}
        self.bind_host_v   = tk.StringVar(value=host)
        self.bind_port_v   = tk.StringVar(value=str(port))
        self.bind_status_v = tk.StringVar(value="(not bound)")

        # Public DoubleVars for the theremin push sliders so the NatNet
        # listener can move them programmatically (which fires the slider's
        # OSC-push callback automatically).
        self.push_therm_pitch_v = tk.DoubleVar(value=440.0)
        self.push_therm_vol_v   = tk.DoubleVar(value=0.5)

        # ---- NatNet (OptiTrack Motive) streaming state ------------------
        # Connection settings
        self.natnet_server_ip_v    = tk.StringVar(value="127.0.0.1")
        self.natnet_client_ip_v    = tk.StringVar(value="127.0.0.1")
        self.natnet_use_multicast_v= tk.BooleanVar(value=True)
        self.natnet_status_v       = tk.StringVar(value="not connected")
        # Mapping settings: which rigid body, which axis -> Pitch/Volume.
        self.rb_id_v       = tk.IntVar(value=1)
        self.pitch_axis_v  = tk.StringVar(value="Y")
        self.pitch_min_v   = tk.StringVar(value="-1.0")
        self.pitch_max_v   = tk.StringVar(value="1.0")
        self.volume_axis_v = tk.StringVar(value="X")
        self.volume_min_v  = tk.StringVar(value="-1.0")
        self.volume_max_v  = tk.StringVar(value="1.0")
        self.natnet_pos_v  = tk.StringVar(value="latest position: -")
        # Latest position written by the NatNet rigid-body listener thread;
        # the Tk main loop drains it inside _tick().
        self._natnet_lock = threading.Lock()
        self._natnet_latest = None  # tuple (rb_id, x, y, z) or None
        self._natnet_client = None  # NatNetClient instance or None

        root.title(f"OSC server @ {host}:{port}")
        root.geometry("680x900")
        root.protocol("WM_DELETE_WINDOW", self._close)
        # Build the UI *first* so that self.log exists before _bind() tries to
        # write status messages into it.
        self._build_ui()

        # Initial bind. If it fails, the dashboard stays up with an error in
        # the bind-status label so the user can fix the address and Rebind.
        self._bind(host, port)

        self._tick()

    # ---- UI ---------------------------------------------------------------

    def _build_ui(self) -> None:
        # --- Role banner -------------------------------------------------
        banner = tk.Label(self.root, text="OSC  SERVER",
                          font=("TkDefaultFont", 16, "bold"),
                          bg="#2E7D32", fg="white", pady=10)
        banner.pack(fill="x")

        # --- Bind address -----------------------------------------------
        addr = tk.LabelFrame(self.root, text="Bind address", padx=8, pady=6)
        addr.pack(fill="x", padx=10, pady=(8, 4))
        tk.Label(addr, text="Host:").pack(side="left")
        tk.Entry(addr, textvariable=self.bind_host_v, width=16,
                 font=("Courier", 10)).pack(side="left", padx=4)
        tk.Label(addr, text="Port:").pack(side="left", padx=(8, 2))
        tk.Entry(addr, textvariable=self.bind_port_v, width=6,
                 font=("Courier", 10)).pack(side="left", padx=4)
        tk.Button(addr, text="Rebind", command=self._rebind).pack(side="left", padx=8)
        tk.Label(addr, textvariable=self.bind_status_v,
                 font=("Courier", 10), fg="#444").pack(side="left", padx=8)

        # Stats line (mirrors the client's "dest_status" line vertically).
        bar = tk.Frame(self.root); bar.pack(fill="x", padx=10, pady=(0, 4))
        tk.Label(bar, text="Last sender:").pack(side="left")
        tk.Label(bar, textvariable=self.last_sender_v,
                 font=("Courier", 10)).pack(side="left")
        tk.Label(bar, text="    Received:").pack(side="left", padx=(12, 2))
        tk.Label(bar, textvariable=self.total_v,
                 font=("Courier", 10)).pack(side="left")

        # --- Sliders (Freq / Gain / LFO) -- push to last sender ---------
        sliders = tk.LabelFrame(self.root,
                                text="Sliders (push to last-seen sender)",
                                padx=8, pady=6)
        sliders.pack(fill="x", padx=10, pady=4)
        self._push_slider(sliders, "/server/freq", 20.0, 2000.0, 440.0,
                          name_var=self.name_freq_v)
        self._push_slider(sliders, "/server/gain",  0.0,    1.0, 0.5,
                          name_var=self.name_gain_v)
        self._push_slider(sliders, "/server/lfo",   0.0,    1.0, 0.0,
                          name_var=self.name_lfo_v)

        # --- Theremin (push to last sender; on/off lives on the client) -
        therm = tk.LabelFrame(self.root,
                              text="Theremin (push to last-seen sender)",
                              padx=8, pady=6)
        therm.pack(fill="x", padx=10, pady=4)
        # Read-only on/off badge mirroring the client's switch. Coloured
        # so it stays legible against the default gray window background:
        # green ON / red OFF / gray "-" (no client yet).
        ctl = tk.Frame(therm); ctl.pack(fill="x", pady=(0, 4))
        tk.Label(ctl, text="On/Off (client-side):").pack(side="left")
        on_badge = tk.Label(ctl, textvariable=self.therm_on_v, width=5,
                            font=("TkDefaultFont", 10, "bold"),
                            bg="#E0E0E0", fg="#333",
                            relief="solid", bd=1, padx=6)
        on_badge.pack(side="left", padx=6)

        def _update_on_badge(*_):
            state = self.therm_on_v.get()
            if state == "ON":
                on_badge.config(bg="#2E7D32", fg="white")   # green
            elif state == "OFF":
                on_badge.config(bg="#C62828", fg="white")   # red
            else:
                on_badge.config(bg="#E0E0E0", fg="#333")    # neutral
        self.therm_on_v.trace_add("write", _update_on_badge)
        _update_on_badge()

        self._push_slider(therm, "/server/theremin/pitch",  80.0, 2000.0, 440.0,
                          display_label="Pitch",
                          var=self.push_therm_pitch_v)
        self._push_slider(therm, "/server/theremin/volume",  0.0,    1.0, 0.5,
                          display_label="Volume",
                          var=self.push_therm_vol_v)

        # --- Action buttons ---------------------------------------------
        btns = tk.LabelFrame(self.root, text="Buttons", padx=8, pady=6)
        btns.pack(fill="x", padx=10, pady=4)
        tk.Button(btns, text="Tell client: Play",
                  command=lambda: self._push("/server/transport/play", True)
                  ).pack(side="left", padx=2)
        tk.Button(btns, text="Tell client: Stop",
                  command=lambda: self._push("/server/transport/stop", True)
                  ).pack(side="left", padx=2)
        tk.Button(btns, text="Ping",
                  command=lambda: self._push("/server/ping", True)
                  ).pack(side="left", padx=2)
        tk.Button(btns, text="Announce",
                  command=lambda: self._push("/server/announce",
                                             "hello from server")
                  ).pack(side="left", padx=2)

        # --- Behaviour --------------------------------------------------
        beh = tk.LabelFrame(self.root, text="Behaviour", padx=8, pady=6)
        beh.pack(fill="x", padx=10, pady=4)
        tk.Checkbutton(beh, text="Echo  (reply with /ack<address>)",
                       variable=self.echo_var).pack(side="left")
        tk.Checkbutton(beh, text="Pause  (drop incoming)",
                       variable=self.paused_var).pack(side="left", padx=14)
        tk.Button(beh, text="Clear log", command=self._clear_log).pack(side="right")

        # --- NatNet (OptiTrack) streaming -------------------------------
        nn = tk.LabelFrame(self.root, text="NatNet streaming (OptiTrack Motive)",
                           padx=8, pady=6)
        nn.pack(fill="x", padx=10, pady=4)

        row1 = tk.Frame(nn); row1.pack(fill="x", pady=2)
        tk.Label(row1, text="Server IP:").pack(side="left")
        tk.Entry(row1, textvariable=self.natnet_server_ip_v, width=14,
                 font=("Courier", 10)).pack(side="left", padx=4)
        tk.Label(row1, text="Client IP:").pack(side="left", padx=(10, 2))
        tk.Entry(row1, textvariable=self.natnet_client_ip_v, width=14,
                 font=("Courier", 10)).pack(side="left", padx=4)
        tk.Radiobutton(row1, text="Multicast", value=True,
                       variable=self.natnet_use_multicast_v).pack(side="left", padx=(12, 2))
        tk.Radiobutton(row1, text="Unicast",   value=False,
                       variable=self.natnet_use_multicast_v).pack(side="left")

        row2 = tk.Frame(nn); row2.pack(fill="x", pady=2)
        self._natnet_btn = tk.Button(row2, text="Connect",
                                     command=self._natnet_toggle, width=12)
        self._natnet_btn.pack(side="left", padx=2)
        tk.Label(row2, textvariable=self.natnet_status_v,
                 font=("Courier", 10), fg="#444").pack(side="left", padx=8)

        # --- NatNet -> Theremin mapping ---------------------------------
        mp = tk.LabelFrame(self.root,
                           text="NatNet -> Theremin mapping (rigid body XYZ -> Pitch / Volume)",
                           padx=8, pady=6)
        mp.pack(fill="x", padx=10, pady=4)

        rrow = tk.Frame(mp); rrow.pack(fill="x", pady=2)
        tk.Label(rrow, text="Rigid body ID:").pack(side="left")
        tk.Spinbox(rrow, from_=1, to=999, width=5,
                   textvariable=self.rb_id_v).pack(side="left", padx=4)
        tk.Label(rrow, textvariable=self.natnet_pos_v,
                 font=("Courier", 10), fg="#444").pack(side="left", padx=10)

        def axis_row(parent, label, axis_var, min_var, max_var):
            row = tk.Frame(parent); row.pack(fill="x", pady=2)
            tk.Label(row, text=label, width=14, anchor="w").pack(side="left")
            tk.Label(row, text="axis:").pack(side="left")
            tk.OptionMenu(row, axis_var, "X", "Y", "Z").pack(side="left", padx=4)
            tk.Label(row, text="range").pack(side="left", padx=(8, 2))
            tk.Entry(row, textvariable=min_var, width=6,
                     font=("Courier", 10)).pack(side="left", padx=2)
            tk.Label(row, text="..").pack(side="left")
            tk.Entry(row, textvariable=max_var, width=6,
                     font=("Courier", 10)).pack(side="left", padx=2)
            tk.Label(row, text="(meters)", fg="#666").pack(side="left", padx=4)

        axis_row(mp, "Pitch from",  self.pitch_axis_v,
                 self.pitch_min_v, self.pitch_max_v)
        axis_row(mp, "Volume from", self.volume_axis_v,
                 self.volume_min_v, self.volume_max_v)

        # Log
        log_frame = tk.LabelFrame(self.root,
                                  text="Log  (-> sent  /  <- received)",
                                  padx=8, pady=6)
        log_frame.pack(fill="both", expand=True, padx=10, pady=(4, 10))
        self.log = scrolledtext.ScrolledText(log_frame, height=14,
                                             font=("Courier", 10),
                                             state="disabled")
        self.log.pack(fill="both", expand=True)
        self.log.tag_config("recv", foreground="#06a")
        self.log.tag_config("sent", foreground="#0a6")
        self.log.tag_config("info", foreground="#666")
        self.log.tag_config("err",  foreground="#c33")

    # ---- NatNet integration ----------------------------------------------

    def _natnet_toggle(self) -> None:
        """Connect/disconnect to a NatNet (Motive) stream."""
        if self._natnet_client is not None:
            self._natnet_disconnect()
        else:
            self._natnet_connect()

    def _natnet_connect(self) -> None:
        try:
            from natnet import NatNetClient
        except Exception as e:
            self._add_log(f"!  NatNet SDK import failed: {e}", "err")
            self.natnet_status_v.set("SDK not importable")
            return
        client = NatNetClient()
        client.set_server_address(self.natnet_server_ip_v.get().strip() or "127.0.0.1")
        client.set_client_address(self.natnet_client_ip_v.get().strip() or "127.0.0.1")
        client.set_use_multicast(bool(self.natnet_use_multicast_v.get()))
        client.rigid_body_listener = self._natnet_rigid_body_cb
        try:
            ok = client.run()
        except Exception as e:
            self._add_log(f"!  NatNet connect failed: {e}", "err")
            self.natnet_status_v.set("connect failed")
            return
        if not ok:
            self._add_log("!  NatNet client.run() returned False", "err")
            self.natnet_status_v.set("connect failed")
            try: client.shutdown()
            except Exception: pass
            return
        self._natnet_client = client
        mode = "multicast" if self.natnet_use_multicast_v.get() else "unicast"
        self.natnet_status_v.set(
            f"connected ({mode}) server={self.natnet_server_ip_v.get()} "
            f"client={self.natnet_client_ip_v.get()}")
        self._natnet_btn.config(text="Disconnect")
        self._add_log(f"[natnet] connected ({mode})", "info")

    def _natnet_disconnect(self) -> None:
        client = self._natnet_client
        self._natnet_client = None
        if client is not None:
            try: client.shutdown()
            except Exception as e: self._add_log(f"!  NatNet shutdown: {e}", "err")
        self._natnet_btn.config(text="Connect")
        self.natnet_status_v.set("not connected")
        self._add_log("[natnet] disconnected", "info")

    def _natnet_rigid_body_cb(self, new_id, position, rotation) -> None:
        """Called from a NatNet background thread, once per rigid body
        per frame. Stash the latest position for the rigid body the user
        selected; the Tk main loop polls it from _tick()."""
        try:
            target = int(self.rb_id_v.get())
        except (TypeError, ValueError, tk.TclError):
            return
        if int(new_id) != target:
            return
        try:
            x, y, z = float(position[0]), float(position[1]), float(position[2])
        except Exception:
            return
        with self._natnet_lock:
            self._natnet_latest = (int(new_id), x, y, z)

    @staticmethod
    def _safe_float(s: str, default: float) -> float:
        try:
            return float(s)
        except (TypeError, ValueError):
            return default

    def _map_axis(self, value: float,
                  in_min: float, in_max: float,
                  out_min: float, out_max: float) -> float:
        """Linear map with clamping. Handles inverted ranges."""
        if in_max == in_min:
            return out_min
        t = (value - in_min) / (in_max - in_min)
        if t < 0.0: t = 0.0
        elif t > 1.0: t = 1.0
        return out_min + t * (out_max - out_min)

    def _apply_natnet_latest(self) -> None:
        """Drain the latest rigid-body position (if any) and fan it out
        into the Pitch and Volume push sliders."""
        with self._natnet_lock:
            latest = self._natnet_latest
            self._natnet_latest = None
        if latest is None:
            return
        rb_id, x, y, z = latest
        coords = {"X": x, "Y": y, "Z": z}
        pa = self.pitch_axis_v.get();   va = self.volume_axis_v.get()
        if pa not in coords or va not in coords:
            return
        pitch = self._map_axis(coords[pa],
                               self._safe_float(self.pitch_min_v.get(), -1.0),
                               self._safe_float(self.pitch_max_v.get(),  1.0),
                               80.0, 2000.0)
        volume = self._map_axis(coords[va],
                                self._safe_float(self.volume_min_v.get(), -1.0),
                                self._safe_float(self.volume_max_v.get(),  1.0),
                                0.0, 1.0)
        # Setting the DoubleVars fires the slider's command, which calls
        # _push to send /server/theremin/pitch and /server/theremin/volume.
        self.push_therm_pitch_v.set(pitch)
        self.push_therm_vol_v.set(volume)
        self.natnet_pos_v.set(
            f"latest position (id={rb_id}): "
            f"X={x:+.3f}  Y={y:+.3f}  Z={z:+.3f}  m")

    def _push_slider(self, parent, address, lo, hi, initial,
                     name_var=None, display_label=None, var=None) -> None:
        """Build one push-slider row.

        Label precedence: `name_var` (editable Entry) -> `display_label`
        (static string for the user) -> `address` (the OSC address as a
        last resort).

        If `var` is given, the Scale is bound to that DoubleVar so callers
        can move the slider programmatically (e.g., from the NatNet
        listener mapping a rigid body's position into Pitch / Volume).
        Setting the var fires the Scale's command, which calls _push, so
        the new value is pushed over OSC the same way as a manual drag.
        """
        row = tk.Frame(parent); row.pack(fill="x", pady=2)
        if name_var is None:
            text = display_label if display_label is not None else address
            tk.Label(row, text=text, width=15, anchor="w"
                     ).pack(side="left")
        else:
            entry = tk.Entry(row, textvariable=name_var, width=15,
                             font=("TkDefaultFont", 10))
            entry.pack(side="left", padx=2)
            slot = address.rsplit("/", 1)[-1]   # "freq" / "gain" / "lfo"
            rename_addr = f"/server/rename/{slot}"

            def commit(_event=None):
                new_name = name_var.get().strip()
                if not new_name:
                    return
                if self._last_sent_names.get(rename_addr) == new_name:
                    return  # no-op rename, don't spam the client
                self._last_sent_names[rename_addr] = new_name
                self._push(rename_addr, new_name)

            entry.bind("<Return>", commit)
            entry.bind("<FocusOut>", commit)

        val_lbl = tk.Label(row, text=f"{initial:.2f}", width=8, anchor="e",
                           font=("Courier", 10))
        val_lbl.pack(side="right")
        if var is None:
            var = tk.DoubleVar(value=initial)
        else:
            var.set(initial)

        # Build the Scale FIRST so the trace below isn't fired by the
        # Scale's internal initialization writes.
        tk.Scale(row, variable=var, from_=lo, to=hi,
                 resolution=(hi - lo) / 200.0,
                 orient="horizontal", showvalue=False
                 ).pack(fill="x", expand=True, side="left", padx=6)

        # Use a variable trace rather than the Scale's `command=`. Tk's
        # Scale only fires its command for user mouse drags; for
        # programmatic `var.set(...)` calls (e.g., the NatNet listener
        # mapping a rigid body's position into Pitch / Volume) the
        # command is suppressed and our _push would never run. A write
        # trace fires for both cases, so the OSC packet goes out
        # whether the slider was moved by hand or by code.
        def on_change(*_args, _addr=address, _lbl=val_lbl, _v=var):
            try:
                val = float(_v.get())
            except (TypeError, ValueError, tk.TclError):
                return
            _lbl.config(text=f"{val:.2f}")
            self._push(_addr, val)
        var.trace_add("write", on_change)


    # ---- Networking -------------------------------------------------------

    def _bind(self, host: str, port: int) -> bool:
        """Open a UDP socket bound to host:port, replacing any current one.
        Returns True on success and starts the recv thread, False on failure
        (the UI is left running so the user can fix the address)."""
        # Tear down any existing socket first; the recv thread will exit on
        # the resulting OSError.
        if self.sock is not None:
            try: self.sock.close()
            except Exception: pass
        if self.recv_thread is not None:
            self.recv_thread.join(timeout=0.5)
            self.recv_thread = None
        self.sock = None

        try:
            new_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            new_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            new_sock.bind((host, port))
            new_sock.settimeout(0.25)
        except Exception as e:
            self.bind_status_v.set(f"BIND FAILED: {e}")
            self._add_log(f"!  bind {host}:{port} failed: {e}", "err")
            return False

        self.sock = new_sock
        self.host = host
        self.port = port
        self.bind_host_v.set(host)
        self.bind_port_v.set(str(port))
        self.bind_status_v.set(f"bound to {host}:{port}")
        self._add_log(f"[server] bound to {host}:{port}", "info")
        self.recv_thread = threading.Thread(target=self._recv_loop, daemon=True)
        self.recv_thread.start()
        return True

    def _rebind(self) -> None:
        host = self.bind_host_v.get().strip() or "127.0.0.1"
        try:
            port = int(self.bind_port_v.get())
            if not (0 <= port <= 65535):
                raise ValueError("out of range")
        except ValueError as e:
            self._add_log(f"!  invalid port {self.bind_port_v.get()!r}: {e}", "err")
            return
        self._bind(host, port)

    def _push(self, address: str, *args) -> None:
        if self.sock is None:
            self._add_log("!  no socket bound yet (use Rebind)", "err")
            return
        if not self.last_sender:
            self._add_log(f"!  no client yet to push '{address}' to", "err")
            return
        try:
            self.sock.sendto(encode_message(address, *args), self.last_sender)
            self._add_log(f"-> {self.last_sender[0]}:{self.last_sender[1]}  "
                          f"{address}  {list(args)}", "sent")
        except Exception as e:
            self._add_log(f"!  push error: {e}", "err")

    def _recv_loop(self) -> None:
        sock = self.sock  # capture; rebind will swap self.sock to a new one
        while self.running and sock is not None:
            try:
                data, addr = sock.recvfrom(65535)
            except socket.timeout:
                if sock is not self.sock:
                    return  # socket was swapped out under us
                continue
            except OSError:
                return  # socket was closed (rebinding or shutdown)
            self.q.put(("packet", addr, data))

    def _tick(self) -> None:
        try:
            while True:
                kind, addr, payload = self.q.get_nowait()
                if kind == "packet":
                    self._handle_packet(addr, payload)
        except queue.Empty:
            pass
        # Apply the latest NatNet rigid-body position (if any) into the
        # Pitch / Volume push sliders.
        self._apply_natnet_latest()
        if self.running:
            self.root.after(40, self._tick)

    def _handle_packet(self, addr, data: bytes) -> None:
        if self.paused_var.get():
            return  # drop while paused
        if self.sock is None:
            return  # mid-rebind

        try:
            address, args = decode_message(data)
        except Exception as e:
            self._add_log(f"!  decode error from {addr}: {e}", "err")
            return

        # Update stats
        self.total += 1
        self.total_v.set(str(self.total))
        self.last_sender = addr
        self.last_sender_v.set(f"{addr[0]}:{addr[1]}")

        # The only piece of incoming state we still surface is the
        # theremin on/off, shown in the Theremin push frame.
        if address == "/theremin/on" and args:
            self.therm_on_v.set("ON" if args[0] else "OFF")

        self._add_log(f"<- {addr[0]}:{addr[1]}  {address}  {args}", "recv")

        # Echo back as /ack<address>
        if self.echo_var.get() and not address.startswith("/ack"):
            try:
                self.sock.sendto(encode_message("/ack" + address, *args), addr)
            except Exception as e:
                self._add_log(f"!  echo error: {e}", "err")

    # ---- Log helpers ------------------------------------------------------

    def _add_log(self, msg: str, tag: str = "info") -> None:
        self.log.configure(state="normal")
        self.log.insert("end", msg + "\n", tag)
        n = int(self.log.index("end-1c").split(".")[0])
        if n > 500:
            self.log.delete("1.0", f"{n - 500}.0")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _clear_log(self) -> None:
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")

    def _close(self) -> None:
        self.running = False
        # Tear down NatNet first so its threads don't keep running.
        if self._natnet_client is not None:
            try: self._natnet_client.shutdown()
            except Exception: pass
            self._natnet_client = None
        try:
            self.sock.close()
        except Exception:
            pass
        self.root.destroy()


def main() -> None:
    p = argparse.ArgumentParser(description="OSC server with Tkinter UI")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=9000)
    args = p.parse_args()

    root = tk.Tk()
    OscServer(root, args.host, args.port)
    root.mainloop()


if __name__ == "__main__":
    main()
