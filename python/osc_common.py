"""
osc_common.py - Minimal OSC 1.0 encoder/decoder using only the Python stdlib.

Implements the subset of the OSC spec most commonly used:
  - Address pattern (OSC-string)
  - Type tag string starting with ','
  - Arguments: i (int32), f (float32), s (OSC-string), b (OSC-blob),
               T (true), F (false), N (nil), I (impulse/infinitum)

OSC strings and blobs are padded with zero bytes to a 4-byte boundary.
All numeric values are big-endian (network byte order).
"""

import struct


def _pad4(data: bytes) -> bytes:
    """Right-pad bytes with NULs so the total length is a multiple of 4."""
    rem = len(data) % 4
    return data if rem == 0 else data + b"\x00" * (4 - rem)


def _osc_string(s: str) -> bytes:
    """Encode an OSC-string: UTF-8 bytes + NUL terminator, padded to 4 bytes."""
    return _pad4(s.encode("utf-8") + b"\x00")


def _osc_blob(b: bytes) -> bytes:
    """Encode an OSC-blob: int32 size prefix + raw bytes, padded to 4 bytes."""
    return _pad4(struct.pack(">i", len(b)) + b)


def encode_message(address: str, *args) -> bytes:
    """Build a single OSC message from an address and Python arguments."""
    if not address.startswith("/"):
        raise ValueError("OSC address must start with '/'")

    typetag = ","
    payload = b""

    for arg in args:
        if arg is True:
            typetag += "T"
        elif arg is False:
            typetag += "F"
        elif arg is None:
            typetag += "N"
        elif isinstance(arg, bool):
            # bool is subclass of int; handled above, but be explicit.
            typetag += "T" if arg else "F"
        elif isinstance(arg, int):
            typetag += "i"
            payload += struct.pack(">i", arg)
        elif isinstance(arg, float):
            typetag += "f"
            payload += struct.pack(">f", arg)
        elif isinstance(arg, str):
            typetag += "s"
            payload += _osc_string(arg)
        elif isinstance(arg, (bytes, bytearray)):
            typetag += "b"
            payload += _osc_blob(bytes(arg))
        else:
            raise TypeError(f"Unsupported OSC argument type: {type(arg).__name__}")

    return _osc_string(address) + _osc_string(typetag) + payload


def _read_string(data: bytes, offset: int):
    """Read a NUL-terminated OSC string starting at offset; return (str, new_offset)."""
    end = data.index(b"\x00", offset)
    s = data[offset:end].decode("utf-8")
    # Advance past the NUL and any padding to the next 4-byte boundary.
    new_offset = (end + 4) & ~3
    return s, new_offset


def decode_message(data: bytes):
    """Decode a single OSC message. Returns (address, [args])."""
    address, offset = _read_string(data, 0)
    typetag, offset = _read_string(data, offset)

    if not typetag.startswith(","):
        raise ValueError("Malformed OSC message: type tag must start with ','")

    args = []
    for tag in typetag[1:]:
        if tag == "i":
            (val,) = struct.unpack(">i", data[offset:offset + 4])
            args.append(val)
            offset += 4
        elif tag == "f":
            (val,) = struct.unpack(">f", data[offset:offset + 4])
            args.append(val)
            offset += 4
        elif tag == "s":
            val, offset = _read_string(data, offset)
            args.append(val)
        elif tag == "b":
            (size,) = struct.unpack(">i", data[offset:offset + 4])
            offset += 4
            args.append(data[offset:offset + size])
            offset += size
            offset = (offset + 3) & ~3  # pad to next 4-byte boundary
        elif tag == "T":
            args.append(True)
        elif tag == "F":
            args.append(False)
        elif tag == "N":
            args.append(None)
        elif tag == "I":
            args.append(float("inf"))
        else:
            raise ValueError(f"Unsupported OSC type tag: {tag!r}")

    return address, args
