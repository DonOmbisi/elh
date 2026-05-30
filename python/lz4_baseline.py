"""ctypes wrapper for the clean upstream LZ4 baseline.

Set LZ4_LIBRARY=/path/to/liblz4_clean.so (or lz4_clean.dll on Windows) to
override discovery. The library is built from third_party/lz4, not from any
modified sibling checkout.
"""

from __future__ import annotations

import ctypes
import os
import sys
from pathlib import Path


class Error(RuntimeError):
    """Raised when the LZ4 baseline library reports an error."""


_lib = None


def _candidate_libraries() -> list[Path | str]:
    env = os.environ.get("LZ4_LIBRARY")
    if env:
        return [Path(env)]

    here = Path(__file__).resolve()
    repo = here.parent.parent
    if sys.platform.startswith("win"):
        names = ["lz4_clean.dll", "liblz4_clean.dll"]
    elif sys.platform == "darwin":
        names = ["liblz4_clean.dylib", "liblz4_clean.so"]
    else:
        names = ["liblz4_clean.so", "liblz4_clean.dylib"]

    dirs = [
        repo / "build",
        repo / "build" / "Debug",
        repo / "build" / "Release",
        here.parent,
        Path.cwd(),
    ]
    candidates: list[Path | str] = []
    for directory in dirs:
        for name in names:
            candidates.append(directory / name)
    candidates.extend(names)
    return candidates


def _load_library():
    global _lib
    if _lib is not None:
        return _lib

    errors = []
    for candidate in _candidate_libraries():
        try:
            lib = ctypes.CDLL(str(candidate))
            _configure_library(lib)
            _lib = lib
            return lib
        except OSError as exc:
            errors.append(f"{candidate}: {exc}")

    detail = "\n".join(errors[:8])
    raise Error(
        "could not load clean LZ4 shared library. Build with "
        "`cmake --build build --target lz4_clean_shared` or set LZ4_LIBRARY.\n"
        + detail
    )


def _configure_library(lib) -> None:
    lib.LZ4_compressBound.argtypes = [ctypes.c_int]
    lib.LZ4_compressBound.restype = ctypes.c_int

    lib.LZ4_compress_default.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_int,
    ]
    lib.LZ4_compress_default.restype = ctypes.c_int

    lib.LZ4_decompress_safe.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_int,
    ]
    lib.LZ4_decompress_safe.restype = ctypes.c_int


def _as_bytes(data: bytes | bytearray | memoryview) -> bytes:
    if isinstance(data, bytes):
        return data
    if isinstance(data, (bytearray, memoryview)):
        return bytes(data)
    raise TypeError("data must be bytes-like")


def compress(data: bytes | bytearray | memoryview) -> bytes:
    """Compress bytes into one raw LZ4 block."""

    src = _as_bytes(data)
    if not src:
        return b""

    lib = _load_library()
    bound = lib.LZ4_compressBound(len(src))
    if bound <= 0:
        raise Error("invalid compression bound")

    out = ctypes.create_string_buffer(bound)
    inbuf = ctypes.create_string_buffer(src, len(src))
    n = lib.LZ4_compress_default(inbuf, out, len(src), bound)
    if n <= 0:
        raise Error("compression failed")
    return out.raw[:n]


def decompress(block: bytes | bytearray | memoryview, original_size: int) -> bytes:
    """Decompress one raw LZ4 block using its known original size."""

    src = _as_bytes(block)
    if original_size < 0:
        raise ValueError("original_size must be non-negative")
    if original_size == 0:
        return b""

    lib = _load_library()
    inbuf = ctypes.create_string_buffer(src, len(src))
    out = ctypes.create_string_buffer(original_size)
    n = lib.LZ4_decompress_safe(inbuf, out, len(src), original_size)
    if n < 0:
        raise Error("decompression failed")
    return out.raw[:n]


__all__ = ["Error", "compress", "decompress"]
