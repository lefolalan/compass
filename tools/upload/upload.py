#!/usr/bin/env python3
"""Send a binary frame to the e-paper firmware over Wi-Fi.

A frame file holds the raw content of the whole screen, 400x300 pixels at
1 bpp, row-major, MSB first, a set bit is black ink. That makes 15000 bytes.

    python firmware/tools/upload/upload.py 192.168.1.42 \\
        firmware/tools/upload/assets/test-pattern.bin

The board shows its address and port on the boot screen. This tool needs the
packages of requirements.txt. It compiles fw/proto/epaper.proto on the fly, so
that the .proto file stays the only definition of the protocol.
"""

import argparse
import importlib
import socket
import sys
import tempfile
from pathlib import Path

from google.protobuf import proto
from grpc_tools import protoc

PROTO_DIR = Path(__file__).resolve().parents[2] / "fw" / "proto"
PROTO_FILE = "epaper.proto"

PANEL_WIDTH = 400
PANEL_HEIGHT = 300
FRAME_BYTES = PANEL_WIDTH // 8 * PANEL_HEIGHT

# CONFIG_APP_UPLOAD_PORT in fw/Kconfig.
DEFAULT_PORT = 7400

# The response comes after the full refresh of the panel, which takes about 5 s.
RESPONSE_TIMEOUT_S = 30


class UploadError(Exception):
    """The frame did not reach the panel. The message says why."""


def load_protocol():
    """Compile epaper.proto and return the generated module."""
    with tempfile.TemporaryDirectory() as out_dir:
        status = protoc.main(
            ["protoc", f"--proto_path={PROTO_DIR}", f"--python_out={out_dir}", PROTO_FILE]
        )
        if status != 0:
            raise UploadError(f"protoc could not compile {PROTO_DIR / PROTO_FILE}")

        sys.path.insert(0, out_dir)
        try:
            return importlib.import_module("epaper_pb2")
        finally:
            sys.path.remove(out_dir)


def read_frame(path):
    try:
        frame = path.read_bytes()
    except OSError as error:
        raise UploadError(f"cannot read {path}: {error.strerror}") from error

    if len(frame) != FRAME_BYTES:
        raise UploadError(
            f"{path} holds {len(frame)} bytes, a {PANEL_WIDTH}x{PANEL_HEIGHT} frame at 1 bpp "
            f"is exactly {FRAME_BYTES} bytes"
        )

    return frame


def show_frame(host, port, frame):
    """Send one ShowFrame request. Raises UploadError unless the board confirms the refresh."""
    epaper = load_protocol()

    request = epaper.Request()
    request.show_frame.width = PANEL_WIDTH
    request.show_frame.height = PANEL_HEIGHT
    request.show_frame.pixels = frame

    try:
        with socket.create_connection((host, port), timeout=RESPONSE_TIMEOUT_S) as sock:
            with sock.makefile("rwb") as stream:
                proto.serialize_length_prefixed(request, stream)
                stream.flush()
                response = proto.parse_length_prefixed(epaper.Response, stream)
    except OSError as error:
        raise UploadError(f"exchange with {host}:{port} failed: {error}") from error

    if response is None:
        raise UploadError(f"{host}:{port} closed the connection without answering")

    if response.status != epaper.Response.STATUS_OK:
        status = epaper.Response.Status.Name(response.status)
        raise UploadError(f"the board refused the frame, {status}: {response.detail}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("host", help="IPv4 address of the board, shown on its boot screen")
    parser.add_argument("frame", type=Path, help=f"binary frame file of {FRAME_BYTES} bytes")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="TCP port (default: %(default)s)")
    args = parser.parse_args()

    try:
        show_frame(args.host, args.port, read_frame(args.frame))
    except UploadError as error:
        sys.exit(f"upload failed: {error}")

    print(f"{args.frame.name} is on the panel")


if __name__ == "__main__":
    main()
