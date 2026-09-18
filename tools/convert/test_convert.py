"""Tests of convert.py.

    python -m pytest firmware/tools

Most tests run the tool as the plain file a user launches, then read the frame
bit by bit with ink_at(), which shares no code with the tool. A round trip
through Pillow alone would still pass with the bit order wrong both ways.
"""

import subprocess
import sys
from pathlib import Path

import pytest
from PIL import Image, ImageChops

import convert

# The format is written out again on purpose. Tests that imported these values
# from frame_format.py would still pass with a wrong constant in it.
WIDTH = 400
HEIGHT = 300
ROW_BYTES = 50


def run_tool(*arguments):
    return subprocess.run(
        [sys.executable, convert.__file__, *(str(argument) for argument in arguments)],
        capture_output=True,
        text=True,
    )


def ink_at(frame, x, y):
    return bool(frame[y * ROW_BYTES + x // 8] & (0x80 >> (x % 8)))


def ink_bounding_box(frame):
    inked = [(x, y) for y in range(HEIGHT) for x in range(WIDTH) if ink_at(frame, x, y)]
    columns = [x for x, _ in inked]
    rows = [y for _, y in inked]
    return (min(columns), min(rows), max(columns) + 1, max(rows) + 1)


def ink_ratio(frame):
    return sum(byte.bit_count() for byte in frame) / (WIDTH * HEIGHT)


def save_picture(picture, path, **options):
    picture.save(path, **options)
    return path


def white_page(size=(WIDTH, HEIGHT)):
    return Image.new("L", size, "white")


def convert_to_frame(tmp_path, source, *options):
    frame_path = tmp_path / "frame.bin"
    result = run_tool(source, frame_path, *options)
    assert result.returncode == 0, result.stderr
    return frame_path.read_bytes()


@pytest.mark.parametrize("fit", ["contain", "cover"])
@pytest.mark.parametrize("size", [(640, 480), (1000, 300), (240, 900), (37, 23)])
def test_any_picture_size_gives_a_frame_of_exactly_15000_bytes(tmp_path, size, fit):
    photo = save_picture(Image.linear_gradient("L").resize(size).convert("RGB"), tmp_path / "photo.jpg")

    frame = convert_to_frame(tmp_path, photo, "--fit", fit)

    assert len(frame) == 15000


@pytest.mark.parametrize(
    "black_box, expected_bytes",
    [
        # The first pixel is the MSB of the first byte, and a row takes 50 bytes.
        ((0, 0, 12, 2), {0: 0xFF, 1: 0xF0, 50: 0xFF, 51: 0xF0}),
        # The last pixel of the last row is the LSB of the last byte.
        ((399, 299, 400, 300), {14999: 0x01}),
    ],
)
def test_black_pixels_set_the_documented_bits(tmp_path, black_box, expected_bytes):
    page = white_page()
    page.paste("black", black_box)
    source = save_picture(page, tmp_path / "block.png")

    frame = convert_to_frame(tmp_path, source, "--dither", "none")

    expected = bytearray(15000)
    for offset, value in expected_bytes.items():
        expected[offset] = value
    assert frame == bytes(expected)


def test_frame_and_preview_hold_the_pixels_of_a_black_and_white_picture(tmp_path):
    pattern = Image.new("1", (WIDTH, HEIGHT))
    pattern.putdata([255 * ((x * 7 + y * 13) % 5 != 0) for y in range(HEIGHT) for x in range(WIDTH)])
    source = save_picture(pattern, tmp_path / "pattern.png")
    preview_path = tmp_path / "preview.png"

    frame = convert_to_frame(tmp_path, source, "--dither", "none", "--preview", preview_path)

    for y in range(HEIGHT):
        for x in range(WIDTH):
            assert ink_at(frame, x, y) == (pattern.getpixel((x, y)) == 0), (x, y)

    preview = Image.open(preview_path)
    assert preview.format == "PNG"
    assert ImageChops.difference(preview.convert("1"), pattern).getbbox() is None


def test_preview_is_rebuilt_from_the_frame_bytes():
    frame = bytearray(15000)
    frame[0] = 0x80
    frame[ROW_BYTES + 1] = 0x01

    preview = convert.render_preview(bytes(frame))

    black = [(x, y) for y in range(HEIGHT) for x in range(WIDTH) if preview.getpixel((x, y)) == 0]
    assert black == [(0, 0), (15, 1)]


def test_dithered_frame_survives_a_round_trip_through_the_preview(tmp_path):
    photo = save_picture(Image.radial_gradient("L").resize((640, 480)), tmp_path / "photo.png")

    frame = convert.picture_to_frame(photo, "cover", "floyd-steinberg")
    shown = convert.render_preview(frame)

    assert convert.pack_frame(ImageChops.invert(shown)) == frame


@pytest.mark.parametrize(
    "dither, gray_level, lowest_ratio, highest_ratio",
    [
        # Dithering renders a tone with a matching share of ink, so a dark picture is mostly ink.
        ("floyd-steinberg", 64, 0.72, 0.78),
        ("floyd-steinberg", 191, 0.22, 0.28),
        # A plain threshold sends every pixel of a flat tone the same way.
        ("none", 64, 1.0, 1.0),
        ("none", 191, 0.0, 0.0),
    ],
)
def test_dark_tones_turn_into_ink(tmp_path, dither, gray_level, lowest_ratio, highest_ratio):
    source = save_picture(Image.new("L", (WIDTH, HEIGHT), gray_level), tmp_path / "tone.png")

    frame = convert_to_frame(tmp_path, source, "--dither", dither)

    assert lowest_ratio <= ink_ratio(frame) <= highest_ratio


def test_transparent_areas_stay_white(tmp_path):
    picture = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    picture.paste((0, 0, 0, 255), (0, 0, WIDTH // 2, HEIGHT))
    source = save_picture(picture, tmp_path / "transparent.png")

    frame = convert_to_frame(tmp_path, source, "--dither", "none")

    assert frame == (b"\xff" * 25 + b"\x00" * 25) * HEIGHT


@pytest.mark.parametrize(
    "size, expected_box",
    [
        ((100, 100), (50, 0, 350, 300)),
        ((800, 300), (0, 75, 400, 225)),
        # Too elongated to keep one pixel of height at this scale, which Pillow would refuse.
        ((10000, 1), (0, 149, 400, 150)),
    ],
)
def test_contain_shows_the_whole_picture_between_white_bands(tmp_path, size, expected_box):
    source = save_picture(Image.new("L", size, "black"), tmp_path / "black.png")

    frame = convert_to_frame(tmp_path, source, "--fit", "contain", "--dither", "none")

    assert ink_bounding_box(frame) == expected_box


def test_cover_fills_the_panel_and_crops_around_the_center(tmp_path):
    picture = white_page((WIDTH, 600))
    picture.paste("black", (0, 0, WIDTH, 300))
    source = save_picture(picture, tmp_path / "tall.png")

    frame = convert_to_frame(tmp_path, source, "--fit", "cover", "--dither", "none")

    # The panel shows rows 150 to 449 of the picture, so its upper half is black.
    assert frame == b"\xff" * (150 * ROW_BYTES) + b"\x00" * (150 * ROW_BYTES)


def test_cover_enlarges_a_small_picture(tmp_path):
    source = save_picture(Image.new("L", (40, 40), "black"), tmp_path / "small.png")

    frame = convert_to_frame(tmp_path, source, "--fit", "cover", "--dither", "none")

    assert frame == b"\xff" * 15000


def test_exif_rotation_of_a_camera_is_applied(tmp_path):
    # Orientation 6 asks the viewer for a quarter turn clockwise, which brings
    # the top left corner of the stored picture to the top right of the panel.
    stored = white_page((HEIGHT, WIDTH))
    stored.paste("black", (0, 0, 8, 8))
    exif = Image.Exif()
    exif[0x0112] = 6
    source = save_picture(stored, tmp_path / "portrait.png", exif=exif)

    frame = convert_to_frame(tmp_path, source, "--dither", "none")

    assert ink_bounding_box(frame) == (392, 0, 400, 8)


def make_missing(tmp_path):
    return tmp_path / "missing.png"


def make_text(tmp_path):
    path = tmp_path / "text.png"
    path.write_text("this is not a picture")
    return path


def make_directory(tmp_path):
    path = tmp_path / "directory.png"
    path.mkdir()
    return path


def make_truncated(tmp_path):
    whole = save_picture(Image.radial_gradient("L"), tmp_path / "whole.png")
    path = tmp_path / "truncated.png"
    path.write_bytes(whole.read_bytes()[:2000])
    return path


@pytest.mark.parametrize(
    "make_source, reason",
    [
        (make_missing, "No such file or directory"),
        (make_text, "is not a picture that Pillow can read"),
        (make_directory, "Is a directory"),
        (make_truncated, "truncated"),
    ],
)
def test_unusable_source_fails_with_exit_code_1(tmp_path, make_source, reason):
    source = make_source(tmp_path)
    frame_path = tmp_path / "frame.bin"

    result = run_tool(source, frame_path)

    assert result.returncode == 1
    assert result.stdout == ""
    assert result.stderr.startswith("conversion failed: ")
    assert str(source) in result.stderr
    assert reason in result.stderr
    assert not frame_path.exists()


def test_frame_that_cannot_be_written_fails_with_exit_code_1(tmp_path):
    source = save_picture(white_page(), tmp_path / "page.png")
    frame_path = tmp_path / "no-such-directory" / "frame.bin"

    result = run_tool(source, frame_path)

    assert result.returncode == 1
    assert result.stderr.startswith(f"conversion failed: cannot write {frame_path}: ")


def test_source_is_never_overwritten(tmp_path):
    source = save_picture(white_page(), tmp_path / "page.png")
    before = source.read_bytes()

    result = run_tool(source, source)

    assert result.returncode == 1
    assert result.stderr.startswith("conversion failed: ")
    assert source.read_bytes() == before


def test_pack_frame_refuses_an_image_that_is_not_panel_sized():
    with pytest.raises(ValueError, match="399x300"):
        convert.pack_frame(Image.new("1", (399, HEIGHT)))
