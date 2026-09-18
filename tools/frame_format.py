"""Frame format of the e-paper panel, shared by the host tools.

A frame holds the raw content of the whole screen at 1 bpp, row-major, MSB
first, a set bit is black ink. docs/upload.md is the reference.

This module imports nothing, so that a tool only needs its own packages.
"""

PANEL_WIDTH = 400
PANEL_HEIGHT = 300
FRAME_BYTES = PANEL_WIDTH // 8 * PANEL_HEIGHT
