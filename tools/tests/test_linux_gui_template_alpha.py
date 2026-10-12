"""Bounded PNG alpha evidence, without display or production store access."""
import hashlib
import importlib
import struct
import sys
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1] / "linux-gui"))
png_alpha_summary = importlib.import_module("template_alpha").png_alpha_summary
png_reference = importlib.import_module("visual_fixture").png_reference


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(rows, filtering=0, compressed=None, width=2):
    previous = bytes(width * 4)
    encoded = bytearray()
    for row in rows:
        encoded.append(filtering)
        for x, value in enumerate(row):
            left = row[x - 4] if x >= 4 else 0
            up, upper_left = previous[x], previous[x - 4] if x >= 4 else 0
            predictors = (0, left, up, (left + up) // 2)
            if filtering == 4:
                p = left + up - upper_left
                distances = (abs(p - left), abs(p - up), abs(p - upper_left))
                predictor = (left, up, upper_left)[distances.index(min(distances))]
            else:
                predictor = predictors[filtering] if filtering <= 3 else 0
            encoded.append((value - predictor) & 255)
        previous = row
    header = struct.pack(">IIBBBBB", width, len(rows), 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + \
        chunk(b"IDAT", zlib.compress(encoded) if compressed is None else compressed) + chunk(b"IEND", b"")


class AlphaEvidenceTests(unittest.TestCase):
    def test_opaque_fixture(self):
        summary = png_alpha_summary(png_reference(0))
        self.assertEqual((summary["width"], summary["height"]), (32, 32))
        self.assertEqual(summary["opaquePixels"], 1024)
        self.assertEqual(summary["transparentPixels"], 0)
        self.assertEqual(summary["partialPixels"], 0)

    def test_mask_and_all_filters(self):
        rows = (bytes([10, 20, 30, 0, 40, 50, 60, 255]),
                bytes([70, 80, 90, 128, 100, 110, 120, 255]))
        expected = {"width": 2, "height": 2, "transparentPixels": 1, "opaquePixels": 2,
                    "partialPixels": 1, "alphaSha256": hashlib.sha256(bytes([0, 255, 128, 255])).hexdigest()}
        for filtering in range(5):
            with self.subTest(filtering=filtering):
                self.assertEqual(png_alpha_summary(png(rows, filtering)), expected)

    def test_bad_crc_and_truncated_input(self):
        raw = bytearray(png_reference(0))
        raw[-1] ^= 1
        for data in (bytes(raw), b"not a PNG", png_reference(0)[:-3]):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                png_alpha_summary(data)

    def test_dimension_limit_and_excessive_deflate_output(self):
        for data in (png_reference(0, 513, 2),
                     png((bytes(8),), compressed=zlib.compress(bytes(100000)))):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                png_alpha_summary(data)

    def test_invalid_filter_compression_and_trailing_data(self):
        for data in (png((bytes(8),), filtering=5),
                     png((bytes(8),), compressed=b"bad deflate"),
                     png_reference(0) + b"trailing"):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                png_alpha_summary(data)


if __name__ == "__main__":
    unittest.main()
