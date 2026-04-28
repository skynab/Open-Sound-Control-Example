"""
osc_client.py - OSC client with a small Tkinter UI.

Buttons trigger discrete OSC messages (/hello, /transport/play, /transport/stop,
/synth/note). Sliders for /synth/freq, /mixer/channel/1/gain, /lfo/value send
on every change. The log pane shows messages sent (->) and replies received
(<-) from the server (the included server echoes everything back as /ack...).

Tkinter ships with the Python stdlib, so no extra deps. Just run:

    python3 osc_client.py
    python3 osc_client.py --host 192.168.1.20 --port 9000
"""

import argparse
import queue
import socket
import threading
import tkinter as tk
from tkinter import scrolledtext

from osc_common import decode_message, encode_message
from theremin import Theremin


class OscClient:
    def __init__(self, root: tk.Tk, host: str, port: int) -> None:
        self.root = root
        self.dest = (host, port)

        # Bind to an ephemeral local port so the server's reply can find us.
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", 0))
        self.sock.settimeout(0.25)
        self.local_port = self.sock.getsockname()[1]

        self.q: "queue.Queue[tuple[str, str]]" = queue.Queue()
        self.running = True
        threading.Thread(target=self._recv_loop, daemon=True).start()

        # Editable target host/port
        self.dest_host_v   = tk.StringVar(value=host)
        self.dest_port_v   = tk.StringVar(value=str(port))
        self.dest_status_v = tk.StringVar(
            value=f"sending to {host}:{port}   listening on 127.0.0.1:{self.local_port}")

        # Display names for the three core sliders. The server can rename
        # these via /server/rename/{freq,gain,lfo} messages.
        self.name_freq_v = tk.StringVar(value="Frequency")
        self.name_gain_v = tk.StringVar(value="Gain")
        self.name_lfo_v  = tk.StringVar(value="LFO")

        # Slider values for the three core controls. These are public so
        # the recv path can move the slider when /server/freq, etc. arrive.
        # Setting a Tk Scale variable also fires the Scale's command, so
        # moving the slider this way still sends /synth/freq downstream.
        self.freq_v = tk.DoubleVar(value=440.0)
        self.gain_v = tk.DoubleVar(value=0.75)
        self.lfo_v  = tk.DoubleVar(value=0.5)

        # Digital theremin synth + its UI state. The audio stream starts
        # immediately; the on/off button just gates whether samples are
        # non-zero. Default volume is high enough to be clearly audible.
        self.theremin = Theremin()
        self.therm_pitch_v  = tk.DoubleVar(value=440.0)
        self.therm_volume_v = tk.DoubleVar(value=0.7)
        self.therm_on_v     = tk.BooleanVar(value=False)
        self.therm_status_v = tk.StringVar(value=f"OFF | {self.theremin.backend}")

        root.title(f"OSC client -> {host}:{port}")
        root.geometry("560x660")
        root.protocol("WM_DELETE_WINDOW", self._close)
        self._build_ui(host, port)
        self._tick()  # start polling the recv queue

    # ---- UI construction --------------------------------------------------

    def _build_ui(self, host: str, port: int) -> None:
        # --- Role banner -------------------------------------------------
        banner = tk.Label(self.root, text="OSC  CLIENT",
                          font=("TkDefaultFont", 16, "bold"),
                          bg="#1565C0", fg="white", pady=10)
        banner.pack(fill="x")

        # --- Target address ---------------------------------------------
        addr = tk.LabelFrame(self.root, text="Target address", padx=8, pady=6)
        addr.pack(fill="x", padx=10, pady=(8, 4))
        tk.Label(addr, text="Host:").pack(side="left")
        tk.Entry(addr, textvariable=self.dest_host_v, width=16,
                 font=("Courier", 10)).pack(side="left", padx=4)
        tk.Label(addr, text="Port:").pack(side="left", padx=(8, 2))
        tk.Entry(addr, textvariable=self.dest_port_v, width=6,
                 font=("Courier", 10)).pack(side="left", padx=4)
        tk.Button(addr, text="Apply", command=self._apply_dest).pack(side="left", padx=8)
        tk.Label(self.root, textvariable=self.dest_status_v,
                 anchor="w", font=("TkDefaultFont", 10, "bold")
                 ).pack(fill="x", padx=10, pady=(0, 4))

        # --- Sliders (Freq / Gain / LFO) --------------------------------
        sliders = tk.LabelFrame(self.root, text="Sliders (send on change)",
                                padx=8, pady=6)
        sliders.pack(fill="x", padx=10, pady=4)
        self._slider(sliders, "Frequency", 20.0, 2000.0, 440.0,
                     lambda v: self._send("/synth/freq", float(v)),
                     var=self.freq_v, name_var=self.name_freq_v)
        self._slider(sliders, "Gain", 0.0, 1.0, 0.75,
                     lambda v: self._send("/mixer/channel/1/gain", float(v)),
                     var=self.gain_v, name_var=self.name_gain_v)
        self._slider(sliders, "LFO", 0.0, 1.0, 0.5,
                     lambda v: self._send("/lfo/value", float(v)),
                     var=self.lfo_v, name_var=self.name_lfo_v)

        # --- Theremin (real audio synth on this machine) ----------------
        therm = tk.LabelFrame(self.root,
                              text="Theremin (digital synth on this client)",
                              padx=8, pady=6)
        therm.pack(fill="x", padx=10, pady=4)

        # Big red warning when audio isn't wired up. Without this users
        # often think the synth is broken when really sounddevice just
        # isn't installed.
        if not self.theremin.available:
            warn = tk.Frame(therm, bg="#FFEBEE", bd=1, relief="solid")
            warn.pack(fill="x", pady=(0, 6))
            tk.Label(warn, text="NO SOUND",
                     bg="#FFEBEE", fg="#B71C1C",
                     font=("TkDefaultFont", 11, "bold")
                     ).pack(side="left", padx=8, pady=4)
            msg = self.theremin.install_hint or self.theremin.backend
            tk.Label(warn,
                     text=f"audio backend not available — {msg}",
                     bg="#FFEBEE", fg="#B71C1C",
                     font=("Courier", 10), wraplength=420, justify="left"
                     ).pack(side="left", padx=4, pady=4)

        # On/Off button + Test Sound + status
        ctl = tk.Frame(therm); ctl.pack(fill="x", pady=(0, 4))
        self.therm_btn = tk.Button(ctl, text="Theremin: OFF", width=18,
                                   command=self._toggle_theremin)
        self.therm_btn.pack(side="left", padx=2)
        tk.Button(ctl, text="Test Sound (1s)", command=self._test_sound,
                  state=("normal" if self.theremin.available else "disabled")
                  ).pack(side="left", padx=4)
        tk.Label(ctl, textvariable=self.therm_status_v,
                 font=("Courier", 10), fg="#444").pack(side="left", padx=10)

        self._slider(therm, "Pitch (Hz)", 80.0, 2000.0, 440.0,
                     self._on_therm_pitch, var=self.therm_pitch_v)
        self._slider(therm, "Volume", 0.0, 1.0, 0.7,
                     self._on_therm_volume, var=self.therm_volume_v)

        # --- Action buttons ---------------------------------------------
        btns = tk.LabelFrame(self.root, text="Buttons", padx=8, pady=6)
        btns.pack(fill="x", padx=10, pady=4)
        tk.Button(btns, text="Play",
                  command=lambda: self._send("/transport/play", True)).pack(side="left", padx=2)
        tk.Button(btns, text="Stop",
                  command=lambda: self._send("/transport/stop", True)).pack(side="left", padx=2)
        tk.Button(btns, text='/hello "world"',
                  command=lambda: self._send("/hello", "world")).pack(side="left", padx=2)

        # --- Note trigger -----------------------------------------------
        note_frame = tk.LabelFrame(self.root, text="Note trigger", padx=8, pady=6)
        note_frame.pack(fill="x", padx=10, pady=4)
        tk.Label(note_frame, text="Note:").pack(side="left")
        self.note_var = tk.IntVar(value=60)
        tk.Spinbox(note_frame, from_=0, to=127, width=5,
                   textvariable=self.note_var).pack(side="left", padx=4)
        tk.Label(note_frame, text="Velocity:").pack(side="left", padx=(8, 0))
        self.vel_var = tk.IntVar(value=100)
        tk.Spinbox(note_frame, from_=0, to=127, width=5,
                   textvariable=self.vel_var).pack(side="left", padx=4)
        tk.Button(note_frame, text="Trigger",
                  command=self._trigger_note).pack(side="left", padx=10)

        # --- Behaviour --------------------------------------------------
        beh = tk.LabelFrame(self.root, text="Behaviour", padx=8, pady=6)
        beh.pack(fill="x", padx=10, pady=4)
        tk.Button(beh, text="Clear log", command=self._clear_log
                  ).pack(side="right", padx=2)

        # --- Log --------------------------------------------------------
        log_frame = tk.LabelFrame(self.root,
                                  text="Log  (-> sent  /  <- received)",
                                  padx=8, pady=6)
        log_frame.pack(fill="both", expand=True, padx=10, pady=4)
        self.log = scrolledtext.ScrolledText(log_frame, height=14,
                                             font=("Courier", 10),
                                             state="disabled")
        self.log.pack(fill="both", expand=True)
        self.log.tag_config("sent", foreground="#0a6")
        self.log.tag_config("recv", foreground="#06a")
        self.log.tag_config("err",  foreground="#c33")

    def _slider(self, parent, label, lo, hi, initial, on_change,
                var=None, name_var=None) -> None:
        row = tk.Frame(parent); row.pack(fill="x", pady=2)
        if name_var is not None:
            tk.Label(row, textvariable=name_var, width=18, anchor="w"
                     ).pack(side="left")
        else:
            tk.Label(row, text=label, width=18, anchor="w"
                     ).pack(side="left")
        value_lbl = tk.Label(row, text=f"{initial:.2f}", width=8, anchor="e",
                             font=("Courier", 10))
        value_lbl.pack(side="right")
        if var is None:
            var = tk.DoubleVar(value=initial)

        def cb(v):
            value_lbl.config(text=f"{float(v):.2f}")
            on_change(v)

        tk.Scale(row, variable=var, from_=lo, to=hi,
                 resolution=(hi - lo) / 200.0,
                 orient="horizontal", showvalue=False,
                 command=cb).pack(fill="x", expand=True, side="left", padx=6)

    def _trigger_note(self) -> None:
        self._send("/synth/note",
                   int(self.note_var.get()), int(self.vel_var.get()))

    # ---- Theremin handlers ------------------------------------------------

    def _on_therm_pitch(self, v) -> None:
        pitch = float(v)
        self.theremin.set(pitch=pitch)
        self._send("/theremin/pitch", pitch)

    def _on_therm_volume(self, v) -> None:
        vol = float(v)
        self.theremin.set(volume=vol)
        self._send("/theremin/volume", vol)

    def _toggle_theremin(self) -> None:
        new_on = not self.therm_on_v.get()
        self.therm_on_v.set(new_on)
        self.theremin.set(on=new_on)
        self.therm_btn.config(text=f"Theremin: {'ON ' if new_on else 'OFF'}",
                              relief=("sunken" if new_on else "raised"))
        self.therm_status_v.set(
            f"{'ON ' if new_on else 'OFF'} | "
            f"{self.therm_pitch_v.get():.1f} Hz, "
            f"{self.therm_volume_v.get():.2f}  | {self.theremin.backend}")
        self._send("/theremin/on", new_on)
        if new_on:
            if self.theremin.available:
                self._add_log(f"[theremin] ON  ({self.theremin.backend})", "sent")
            else:
                self._add_log(
                    f"[theremin] ON requested but audio is silent. "
                    f"Install with: {self.theremin.install_hint}", "err")
        else:
            self._add_log("[theremin] OFF", "sent")

    def _test_sound(self) -> None:
        """Play a 1-second 440 Hz beep so you can confirm audio works."""
        if not self.theremin.available:
            self._add_log(
                f"!  no audio backend — {self.theremin.install_hint}", "err")
            return
        # Save current state, force a known-good test tone, then restore.
        prev_pitch, prev_vol, prev_on = self.theremin.snapshot()
        self.theremin.set(pitch=440.0, volume=0.7, on=True)
        self._add_log("[theremin] Test Sound: 440 Hz for 1 second", "sent")

        def _restore():
            self.theremin.set(pitch=prev_pitch, volume=prev_vol, on=prev_on)
        self.root.after(1000, _restore)

    def _therm_status_refresh(self) -> None:
        on = self.therm_on_v.get()
        self.therm_status_v.set(
            f"{'ON ' if on else 'OFF'} | "
            f"{self.therm_pitch_v.get():.1f} Hz, "
            f"{self.therm_volume_v.get():.2f}  | {self.theremin.backend}")

    # ---- Networking -------------------------------------------------------

    def _apply_dest(self) -> None:
        host = self.dest_host_v.get().strip() or "127.0.0.1"
        try:
            port = int(self.dest_port_v.get())
            if not (0 <= port <= 65535):
                raise ValueError("out of range")
        except ValueError as e:
            self._add_log(f"!  invalid port {self.dest_port_v.get()!r}: {e}", "err")
            return
        self.dest = (host, port)
        self.dest_status_v.set(
            f"sending to {host}:{port}   listening on 127.0.0.1:{self.local_port}")
        self._add_log(f"[client] target = {host}:{port}", "sent")

    def _send(self, address: str, *args) -> None:
        try:
            self.sock.sendto(encode_message(address, *args), self.dest)
            self._add_log(f"-> {address}  {list(args)}", "sent")
        except Exception as e:
            self._add_log(f"!  send error: {e}", "err")

    def _recv_loop(self) -> None:
        while self.running:
            try:
                data, _addr = self.sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                return
            try:
                address, args = decode_message(data)
                # Hand off the parsed message so _tick can route it
                # (server-pushed theremin updates need to touch widgets
                # which must happen on the Tk main thread).
                self.q.put(("packet", address, args))
            except Exception as e:
                self.q.put(("err", "", f"!  decode error: {e}"))

    def _tick(self) -> None:
        try:
            while True:
                item = self.q.get_nowait()
                kind = item[0]
                if kind == "packet":
                    _, address, args = item
                    self._handle_remote(address, args)
                    self._add_log(f"<- {address}  {args}", "recv")
                elif kind == "err":
                    self._add_log(item[2], "err")
        except queue.Empty:
            pass
        if self.running:
            self.root.after(50, self._tick)

    def _handle_remote(self, address: str, args) -> None:
        """Apply server-pushed updates to the local synth + UI sliders."""
        # Server-pushed values for the three core sliders. Setting the
        # Tk var also moves the Scale and fires its command, which sends
        # the equivalent /synth/* message back -- so the server's
        # mirrored-state panel reflects the new value too.
        if address == "/server/freq" and args:
            try:    self.freq_v.set(float(args[0]))
            except (TypeError, ValueError): pass
            return
        if address == "/server/gain" and args:
            try:    self.gain_v.set(float(args[0]))
            except (TypeError, ValueError): pass
            return
        if address == "/server/lfo" and args:
            try:    self.lfo_v.set(float(args[0]))
            except (TypeError, ValueError): pass
            return

        if address == "/server/theremin/pitch" and args:
            try:
                pitch = float(args[0])
            except (TypeError, ValueError):
                return
            self.therm_pitch_v.set(pitch)
            self.theremin.set(pitch=pitch)
            self._therm_status_refresh()
        elif address == "/server/theremin/volume" and args:
            try:
                vol = float(args[0])
            except (TypeError, ValueError):
                return
            self.therm_volume_v.set(vol)
            self.theremin.set(volume=vol)
            self._therm_status_refresh()
        elif address == "/server/rename/freq" and args:
            self.name_freq_v.set(str(args[0]))
        elif address == "/server/rename/gain" and args:
            self.name_gain_v.set(str(args[0]))
        elif address == "/server/rename/lfo" and args:
            self.name_lfo_v.set(str(args[0]))

    # ---- Log helpers ------------------------------------------------------

    def _add_log(self, msg: str, tag: str = "sent") -> None:
        self.log.configure(state="normal")
        self.log.insert("end", msg + "\n", tag)
        # Cap at ~500 lines so long sessions stay snappy.
        line_count = int(self.log.index("end-1c").split(".")[0])
        if line_count > 500:
            self.log.delete("1.0", f"{line_count - 500}.0")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _clear_log(self) -> None:
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")

    # ---- Shutdown ---------------------------------------------------------

    def _close(self) -> None:
        self.running = False
        try:
            self.theremin.close()
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass
        self.root.destroy()


def main() -> None:
    p = argparse.ArgumentParser(description="OSC client with Tkinter UI")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=9000)
    args = p.parse_args()

    root = tk.Tk()
    OscClient(root, args.host, args.port)
    root.mainloop()


if __name__ == "__main__":
    main()
