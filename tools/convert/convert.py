#!/usr/bin/env python3
"""Convert a picture into a binary frame for the e-paper firmware.

A frame file holds the raw content of the whole screen, 400x300 pixels at
1 bpp, row-major, MSB first, a set bit is black ink. That makes 15000 bytes,
ready for tools/upload/upload.py.

    python firmware/tools/convert/convert.py photo.jpg photo.bin --preview photo.png

The picture keeps its proportions, and the frame is always upright, because the
firmware applies CONFIG_APP_DISPLAY_FLIP_180 itself. This tool needs the
packages of requirements.txt.
"""

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageOps, UnidentifiedImageError

TOOLS_DIR = Path(__file__).resolve().parents[1]

# The tools run as plain files, so the modules they share are found by path.
# fw/scripts/img2c.py holds the conversion that the firmware build runs too.
sys.path.insert(0, str(TOOLS_DIR))
sys.path.insert(0, str(TOOLS_DIR.parent / "fw" / "scripts"))

from frame_format import FRAME_BYTES, PANEL_HEIGHT, PANEL_WIDTH
from img2c import load_gray, to_ink

PANEL_SIZE = (PANEL_WIDTH, PANEL_HEIGHT)


class ConvertError(Exception):
    """No usable frame was written. The message says why."""


def contain(gray):
    """Scale the whole picture into the panel, centered between white bands."""
    scale = min(PANEL_WIDTH / gray.width, PANEL_HEIGHT / gray.height)
    # The short side of a very elongated picture would round to 0 pixel, which Pillow refuses.
    width = max(1, round(gray.width * scale))
    height = max(1, round(gray.height * scale))

    page = Image.new("L", PANEL_SIZE, "white")
    page.paste(
        gray.resize((width, height), Image.Resampling.LANCZOS),
        ((PANEL_WIDTH - width) // 2, (PANEL_HEIGHT - height) // 2),
    )
    return page


def cover(gray):
    """Scale the picture until it fills the panel, and crop around its center."""
    return ImageOps.fit(gray, PANEL_SIZE, Image.Resampling.LANCZOS)


FITS = {"contain": contain, "cover": cover}

DITHERS = {
    "floyd-steinberg": Image.Dither.FLOYDSTEINBERG,
    "none": Image.Dither.NONE,
}


def read_picture(path):
    try:
        return load_gray(path)
    except UnidentifiedImageError as error:
        raise ConvertError(f"{path} is not a picture that Pillow can read") from error
    except Image.DecompressionBombError as error:
        raise ConvertError(f"{path} holds too many pixels to be decoded safely") from error
    except OSError as error:
        # Pillow reports a truncated file with an OSError that carries no strerror.
        raise ConvertError(f"cannot read {path}: {error.strerror or error}") from error


def pack_frame(ink):
    """Return the frame bytes of a panel-sized mode "1" image where a set pixel is black ink."""
    if ink.mode != "1" or ink.size != PANEL_SIZE:
        raise ValueError(
            f"a frame comes from a {PANEL_WIDTH}x{PANEL_HEIGHT} image in mode 1, "
            f"not from {ink.width}x{ink.height} in mode {ink.mode}"
        )

    # Pillow packs mode "1" row by row, MSB first, and pads each row to a whole
    # byte. The panel width is a multiple of 8, so no padding bit gets in.
    return ink.tobytes()


def render_preview(frame):
    """Return the picture that the panel shows for these frame bytes."""
    return ImageChops.invert(Image.frombytes("1", PANEL_SIZE, frame))


def picture_to_frame(source, fit, dither):
    """Return the frame bytes of a picture file. fit and dither are keys of FITS and DITHERS."""
    return pack_frame(to_ink(FITS[fit](read_picture(source)), DITHERS[dither]))


def write_frame(path, frame):
    try:
        path.write_bytes(frame)
    except OSError as error:
        raise ConvertError(f"cannot write {path}: {error.strerror}") from error


def write_preview(path, frame_path):
    # The preview is drawn from the frame file itself, so that it proves what the file holds.
    try:
        frame = frame_path.read_bytes()
    except OSError as error:
        raise ConvertError(f"cannot read back {frame_path}: {error.strerror}") from error

    try:
        render_preview(frame).save(path, format="PNG")
    except OSError as error:
        raise ConvertError(f"cannot write {path}: {error.strerror or error}") from error


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", type=Path, help="picture in any format Pillow reads")
    parser.add_argument("frame", type=Path, help=f"binary frame file to write, {FRAME_BYTES} bytes")
    parser.add_argument(
        "--fit",
        choices=FITS,
        default="contain",
        help="contain shows the whole picture between white bands, cover fills the screen "
        "and crops what sticks out (default: %(default)s)",
    )
    parser.add_argument(
        "--dither",
        choices=DITHERS,
        default="floyd-steinberg",
        help="none applies a plain threshold, for a picture that is already black and white "
        "such as text or a logo (default: %(default)s)",
    )
    parser.add_argument(
        "--preview",
        type=Path,
        metavar="PNG",
        help="also write the frame as a PNG picture, rebuilt from the frame file",
    )
    args = parser.parse_args()

    paths = [args.source, args.frame] + ([args.preview] if args.preview else [])

    try:
        # Writing the frame over the source would destroy the picture of the user.
        if len({path.resolve() for path in paths}) != len(paths):
            raise ConvertError("the source, the frame and the preview must be different files")

        write_frame(args.frame, picture_to_frame(args.source, args.fit, args.dither))
        if args.preview:
            write_preview(args.preview, args.frame)
    except ConvertError as error:
        sys.exit(f"conversion failed: {error}")

    print(f"{args.frame.name} holds the frame of {args.source.name}")


if __name__ == "__main__":
    main()
