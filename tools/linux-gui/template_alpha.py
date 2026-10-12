"""Bounded, read-only alpha evidence for runner-owned RGBA8 PNG assets.

This test helper is not a production decoder. The canonical encoder emits
non-interlaced RGBA8; reject other layouts rather than infer mask evidence.
"""
import hashlib
import struct
import zlib

MAX_BYTES = 4 * 1024 * 1024
MAX_DIMENSION = 512


def png_alpha_summary(raw: bytes) -> dict[str, object]:
    if len(raw) > MAX_BYTES or not raw.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("invalid or oversized PNG")
    offset, header, compressed, ended = 8, None, bytearray(), False
    while offset < len(raw):
        if offset + 12 > len(raw):
            raise ValueError("truncated PNG chunk")
        size = struct.unpack_from(">I", raw, offset)[0]
        end = offset + 12 + size
        if end > len(raw):
            raise ValueError("truncated PNG payload")
        kind = raw[offset + 4:offset + 8]
        data = raw[offset + 8:end - 4]
        crc = struct.unpack_from(">I", raw, end - 4)[0]
        if zlib.crc32(kind + data) != crc:
            raise ValueError("PNG checksum mismatch")
        if kind == b"IHDR":
            if header is not None or offset != 8 or size != 13:
                raise ValueError("invalid PNG header")
            width, height, depth, color, compression, filtering, interlace = \
                struct.unpack(">IIBBBBB", data)
            if not (1 <= width <= MAX_DIMENSION and 1 <= height <= MAX_DIMENSION) or \
                    (depth, color, compression, filtering, interlace) != (8, 6, 0, 0, 0):
                raise ValueError("unsupported or oversized PNG layout")
            header = (width, height)
        elif kind == b"IDAT":
            if header is None:
                raise ValueError("PNG data before header")
            compressed.extend(data)
        elif kind == b"IEND":
            if header is None or size != 0 or end != len(raw):
                raise ValueError("invalid PNG end")
            ended = True
        elif not kind[0] & 0x20:
            raise ValueError("unsupported critical PNG chunk")
        offset = end
    if not ended or header is None:
        raise ValueError("incomplete PNG")
    width, height = header
    stride = width * 4
    expected = (stride + 1) * height
    stream = zlib.decompressobj()
    try:
        decoded = stream.decompress(bytes(compressed), expected + 1)
    except zlib.error as error:
        raise ValueError("invalid PNG compressed data") from error
    if len(decoded) != expected or not stream.eof or stream.unconsumed_tail or stream.unused_data:
        raise ValueError("invalid or excessive PNG decoded data")
    previous = bytearray(stride)
    alpha = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        filtering = decoded[start]
        row = bytearray(decoded[start + 1:start + 1 + stride])
        if filtering > 4:
            raise ValueError("unsupported PNG row filter")
        for x in range(stride):
            left = row[x - 4] if x >= 4 else 0
            up = previous[x]
            upper_left = previous[x - 4] if x >= 4 else 0
            if filtering == 1:
                predictor = left
            elif filtering == 2:
                predictor = up
            elif filtering == 3:
                predictor = (left + up) // 2
            elif filtering == 4:
                p = left + up - upper_left
                distances = (abs(p - left), abs(p - up), abs(p - upper_left))
                predictor = (left, up, upper_left)[distances.index(min(distances))]
            else:
                predictor = 0
            row[x] = (row[x] + predictor) & 255
        alpha.extend(row[3::4])
        previous = row
    transparent, opaque = alpha.count(0), alpha.count(255)
    return {"width": width, "height": height, "transparentPixels": transparent,
            "opaquePixels": opaque, "partialPixels": len(alpha) - transparent - opaque,
            "alphaSha256": hashlib.sha256(alpha).hexdigest()}
