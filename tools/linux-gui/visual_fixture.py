"""Small, deterministic PNG inputs for the disposable visual walkthrough only."""
import struct
import sys
import zlib
from pathlib import Path


def png_reference(variant: int, width: int = 32, height: int = 32) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    rows = bytearray()
    for y in range(height):
        rows.append(0)  # PNG filter: none.
        for x in range(width):
            light = ((x // 4 + y // 4) % 2) * 180 + 30
            rows.extend((light, (light + variant * 31) % 256, (x * 7 + variant * 17) % 256, 255))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")


def write_inputs(directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    for variant in range(5):
        (directory / f"reference-{variant}.png").write_bytes(png_reference(variant))
    (directory / "invalid-reference.png").write_bytes(b"Not a PNG")
    (directory / "oversize-reference.png").write_bytes(png_reference(0, 513, 2))


if __name__ == "__main__":
    write_inputs(Path(sys.argv[1]))
