"""Python bindings for the ELH frame API.

Set ELH_LIBRARY=/path/to/libelh.so (or elh.dll on Windows) to override
library discovery. From a source checkout, running from the repository root
after `cmake --build build` is enough for the default loader.
"""

from __future__ import annotations

import ctypes
import os
import sys
from pathlib import Path


DEFAULT_CHUNK_SIZE = 65536


class Error(RuntimeError):
    """Raised when the ELH native library reports an error."""


class _ElhParams(ctypes.Structure):
    _fields_ = [
        ("bucket_k", ctypes.c_int),
        ("use_overflow", ctypes.c_int),
        ("acceleration", ctypes.c_int),
        ("window_size", ctypes.c_int),
        ("use_wide_offsets", ctypes.c_int),
    ]


class _FrameParams(ctypes.Structure):
    _fields_ = [
        ("params", _ElhParams),
        ("chunk_size", ctypes.c_int),
        ("store_uncompressed", ctypes.c_int),
    ]


_lib = None


def _candidate_libraries() -> list[Path | str]:
    env = os.environ.get("ELH_LIBRARY")
    if env:
        return [Path(env)]

    here = Path(__file__).resolve()
    repo = here.parent.parent
    names = []
    if sys.platform.startswith("win"):
        names = ["elh.dll", "libelh.dll"]
    elif sys.platform == "darwin":
        names = ["libelh.dylib", "libelh.so"]
    else:
        names = ["libelh.so", "libelh.dylib"]

    dirs = [
        repo / "build",
        repo / "build" / "Debug",
        repo / "build" / "Release",
        here.parent,
        Path.cwd(),
    ]
    candidates: list[Path | str] = []
    for d in dirs:
        for name in names:
            candidates.append(d / name)
    candidates.extend(names)
    return candidates


def _load_library():
    global _lib
    if _lib is not None:
        return _lib

    errors = []
    for candidate in _candidate_libraries():
        try:
            path = str(candidate)
            lib = ctypes.CDLL(path)
            _configure_library(lib)
            _lib = lib
            return lib
        except OSError as exc:
            errors.append(f"{candidate}: {exc}")

    detail = "\n".join(errors[:8])
    raise Error(
        "could not load ELH shared library. Build with `cmake --build build` "
        "or set ELH_LIBRARY.\n" + detail
    )


def _configure_library(lib) -> None:
    lib.elh_frame_compress_bound.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.elh_frame_compress_bound.restype = ctypes.c_int

    lib.elh_frame_compress.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.c_int,
        _FrameParams,
    ]
    lib.elh_frame_compress.restype = ctypes.c_int

    lib.elh_frame_decompress.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.c_int,
    ]
    lib.elh_frame_decompress.restype = ctypes.c_int

    lib.elh_frame_get_original_size.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.elh_frame_get_original_size.restype = ctypes.c_int


def _as_bytes(data: bytes | bytearray | memoryview) -> bytes:
    if isinstance(data, bytes):
        return data
    if isinstance(data, (bytearray, memoryview)):
        return bytes(data)
    raise TypeError("data must be bytes-like")


def _frame_params(
    *,
    bucket_k: int = 4,
    use_overflow: int = 1,
    acceleration: int = 1,
    window_size: int = 0,
    use_wide_offsets: int = 0,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
    store_uncompressed: bool = True,
) -> _FrameParams:
    return _FrameParams(
        _ElhParams(
            int(bucket_k),
            int(use_overflow),
            int(acceleration),
            int(window_size),
            int(use_wide_offsets),
        ),
        int(chunk_size),
        1 if store_uncompressed else 0,
    )


def compress(
    data: bytes | bytearray | memoryview,
    *,
    bucket_k: int = 4,
    use_overflow: int = 1,
    acceleration: int = 1,
    window_size: int = 0,
    use_wide_offsets: int = 0,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
    store_uncompressed: bool = True,
) -> bytes:
    """Compress bytes into an ELH frame."""

    src = _as_bytes(data)
    lib = _load_library()
    params = _frame_params(
        bucket_k=bucket_k,
        use_overflow=use_overflow,
        acceleration=acceleration,
        window_size=window_size,
        use_wide_offsets=use_wide_offsets,
        chunk_size=chunk_size,
        store_uncompressed=store_uncompressed,
    )
    bound = lib.elh_frame_compress_bound(len(src), params.chunk_size)
    if bound < 0:
        raise Error("invalid compression bound")

    out = ctypes.create_string_buffer(bound)
    inbuf = ctypes.create_string_buffer(src, len(src))
    n = lib.elh_frame_compress(inbuf, len(src), out, bound, params)
    if n < 0:
        raise Error("compression failed")
    return out.raw[:n]


def original_size(frame: bytes | bytearray | memoryview) -> int:
    """Return the decompressed size recorded in an ELH frame."""

    src = _as_bytes(frame)
    lib = _load_library()
    inbuf = ctypes.create_string_buffer(src, len(src))
    n = lib.elh_frame_get_original_size(inbuf, len(src))
    if n < 0:
        raise Error("invalid ELH frame")
    return int(n)


def decompress(frame: bytes | bytearray | memoryview) -> bytes:
    """Decompress an ELH frame into bytes."""

    src = _as_bytes(frame)
    lib = _load_library()
    inbuf = ctypes.create_string_buffer(src, len(src))
    out_size = lib.elh_frame_get_original_size(inbuf, len(src))
    if out_size < 0:
        raise Error("invalid ELH frame")

    out = ctypes.create_string_buffer(out_size if out_size else 1)
    n = lib.elh_frame_decompress(inbuf, len(src), out, out_size)
    if n < 0:
        raise Error("decompression failed")
    return out.raw[:n]


__all__ = ["DEFAULT_CHUNK_SIZE", "Error", "compress", "decompress", "original_size"]
