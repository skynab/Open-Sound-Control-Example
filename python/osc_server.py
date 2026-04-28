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

        root.title(f"OSC server @ {host}:{port}")
        root.geometry("640x720")
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
                          display_label="Pitch")
        self._push_slider(therm, "/server/theremin/volume",  0.0,    1.0, 0.5,
                          display_label="Volume")

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

    def _push_slider(self, parent, address, lo, hi, initial,
                     name_var=None) -> None:
        """Build one push-slider row.

        If `name_var` is provided, the label on the left becomes an
        editable Entry bound to that StringVar. Pressing Enter or
        clicking away commits the new name and pushes a
        /server/rename/<slot> message to the last-seen client.
        """
        row = tk.Frame(parent); row.pack(fill="x", pady=2)
        if name_var is None:
            tk.Label(row, text=address, width=15, anchor="w"
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
        var = tk.DoubleVar(value=initial)

        def cb(v):
            val_lbl.config(text=f"{float(v):.2f}")
            self._push(address, float(v))

        tk.Scale(row, variable=var, from_=lo, to=hi,
                 resolution=(hi - lo) / 200.0,
                 orient="horizontal", showvalue=False,
                 command=cb).pack(fill="x", expand=True, side="left", padx=6)


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
