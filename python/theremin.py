"""
theremin.py - Tiny digital theremin synth.

Generates a continuous sine wave at the current pitch and volume on the
default audio output device. The pitch, volume, and on/off state can be
changed at any time from any thread (lock-protected).

Uses sounddevice + numpy when available; falls back to a silent stub
otherwise (the rest of the app keeps working — you just hear nothing).

Install audio support with:
    pip install sounddevice numpy
"""

import math
import threading


class Theremin:
    """
    Sine-wave theremin. Construct one, then call set(pitch=..., volume=...,
    on=...) to update parameters live. Always call close() when done.

    Attributes after construction:
        available  True if real audio output is wired up, False if silent.
        backend    Short string describing what's playing (or "silent").
    """

    def __init__(self, sample_rate: int = 44100, max_amp: float = 0.5) -> None:
        self.sample_rate = sample_rate
        self.max_amp = max_amp        # cap so a stuck "on" doesn't blow ears
        self.pitch = 440.0            # Hz
        self.volume = 0.5             # 0..1
        self.on = False
        self._phase = 0.0
        self._lock = threading.Lock()
        self._stream = None
        self.available = False
        # `backend` is a one-line, user-visible string. `install_hint` is
        # an actionable second line for the UI to surface when audio is
        # NOT available.
        self.backend = "silent"
        self.install_hint = ""

        try:
            import sounddevice as sd
            import numpy as np
        except ImportError as e:
            self._sd = None
            self._np = None
            self.backend = f"SILENT ({e.name} not installed)"
            self.install_hint = "pip install sounddevice numpy"
            return

        self._sd = sd
        self._np = np
        try:
            self._stream = sd.OutputStream(
                samplerate=sample_rate, channels=1, dtype="float32",
                blocksize=512, callback=self._audio_cb,
            )
            self._stream.start()
        except Exception as e:
            self._stream = None
            self.backend = f"SILENT (audio device error: {e})"
            self.install_hint = ("Check your default audio output device, "
                                 "or run: python3 -m sounddevice")
            return

        self.available = True
        try:
            dev = sd.query_devices(kind="output")
            dev_name = dev["name"] if isinstance(dev, dict) else str(dev)
        except Exception:
            dev_name = "default"
        self.backend = f"sounddevice @ {sample_rate} Hz [{dev_name}]"

    # ---- public control API ----

    def set(self, *, pitch: float | None = None,
            volume: float | None = None,
            on: bool | None = None) -> None:
        with self._lock:
            if pitch is not None:
                self.pitch = max(20.0, min(20000.0, float(pitch)))
            if volume is not None:
                self.volume = max(0.0, min(1.0, float(volume)))
            if on is not None:
                self.on = bool(on)

    def snapshot(self) -> tuple[float, float, bool]:
        with self._lock:
            return (self.pitch, self.volume, self.on)

    def close(self) -> None:
        if self._stream is not None:
            try:
                self._stream.stop()
                self._stream.close()
            except Exception:
                pass
            self._stream = None

    # ---- sounddevice callback (runs on the audio thread) ----

    def _audio_cb(self, outdata, frames, time_info, status) -> None:
        try:
            with self._lock:
                pitch = self.pitch
                vol = (self.volume * self.max_amp) if self.on else 0.0
                phase = self._phase

            np = self._np
            twopi = 2.0 * math.pi
            inc = twopi * pitch / self.sample_rate

            if vol <= 0.0:
                outdata.fill(0.0)
            else:
                idx = np.arange(frames, dtype=np.float64)
                samples = np.sin(phase + idx * inc) * vol
                outdata[:, 0] = samples.astype(np.float32)

            # Always advance phase so that toggling on/off doesn't pop.
            new_phase = (phase + frames * inc) % twopi
            with self._lock:
                self._phase = new_phase
        except Exception:
            # Never raise out of an audio callback; sounddevice will
            # tear the stream down if we do. Just emit silence.
            outdata.fill(0.0)
