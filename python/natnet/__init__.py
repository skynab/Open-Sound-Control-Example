"""Vendored OptiTrack NatNet SDK (Python).

Re-export the NatNetClient class so callers can write:

    from natnet import NatNetClient

The original SDK files (NatNetClient.py, DataDescriptions.py, MoCapData.py)
are kept verbatim except for two minor patches:

  * NatNetClient.py: removed an accidental `from matplotlib.pylab import f`
    (the symbol was never used).
  * NatNetClient.py: changed the sibling imports
    `import DataDescriptions` / `import MoCapData` to package-relative form
    `from . import ...` so this folder works as a sub-package.
  * MoCapData.py: removed an accidental `from re import S` (also unused).
"""

from .NatNetClient import NatNetClient

__all__ = ["NatNetClient"]
