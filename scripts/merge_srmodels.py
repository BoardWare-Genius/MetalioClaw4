#!/usr/bin/env python3
"""Merge supplemental ESP-SR models into a primary custom model package."""

import argparse
import struct
from collections import OrderedDict
from pathlib import Path


NAME_BYTES = 32


def decode_name(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8")


def encode_name(value: str) -> bytes:
    raw = value.encode("utf-8")
    if len(raw) > NAME_BYTES:
        raise ValueError(f"model entry name is too long: {value}")
    return raw.ljust(NAME_BYTES, b"\0")


def read_u32(data: bytes, offset: int) -> tuple[int, int]:
    if offset + 4 > len(data):
        raise ValueError("truncated ESP-SR model package")
    return struct.unpack_from("<I", data, offset)[0], offset + 4


def parse_package(path: Path) -> OrderedDict[str, OrderedDict[str, bytes]]:
    data = path.read_bytes()
    model_count, offset = read_u32(data, 0)
    entries: list[tuple[str, list[tuple[str, int, int]]]] = []
    for _ in range(model_count):
        if offset + NAME_BYTES > len(data):
            raise ValueError(f"truncated model header in {path}")
        model_name = decode_name(data[offset:offset + NAME_BYTES])
        offset += NAME_BYTES
        file_count, offset = read_u32(data, offset)
        files: list[tuple[str, int, int]] = []
        for _ in range(file_count):
            if offset + NAME_BYTES > len(data):
                raise ValueError(f"truncated file header in {path}")
            file_name = decode_name(data[offset:offset + NAME_BYTES])
            offset += NAME_BYTES
            start, offset = read_u32(data, offset)
            length, offset = read_u32(data, offset)
            if start + length > len(data):
                raise ValueError(f"invalid data range for {model_name}/{file_name}")
            files.append((file_name, start, length))
        entries.append((model_name, files))

    models: OrderedDict[str, OrderedDict[str, bytes]] = OrderedDict()
    for model_name, files in entries:
        models[model_name] = OrderedDict(
            (file_name, data[start:start + length])
            for file_name, start, length in files
        )
    return models


def write_package(
    path: Path, models: OrderedDict[str, OrderedDict[str, bytes]]
) -> None:
    file_count = sum(len(files) for files in models.values())
    header_size = 4 + len(models) * (NAME_BYTES + 4) + file_count * (
        NAME_BYTES + 4 + 4
    )
    header = bytearray(struct.pack("<I", len(models)))
    payload = bytearray()
    for model_name, files in models.items():
        header.extend(encode_name(model_name))
        header.extend(struct.pack("<I", len(files)))
        for file_name, contents in files.items():
            header.extend(encode_name(file_name))
            header.extend(struct.pack("<II", header_size + len(payload), len(contents)))
            payload.extend(contents)
    if len(header) != header_size:
        raise AssertionError("ESP-SR package header size mismatch")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + payload)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--primary", type=Path, required=True)
    parser.add_argument("--supplemental", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    models = parse_package(args.primary)
    supplemental = parse_package(args.supplemental)
    for model_name, files in supplemental.items():
        # The primary package owns the custom wake word.  Do not reintroduce
        # the sdkconfig-selected stock WakeNet model alongside it.
        if model_name.lower().startswith("wn"):
            continue
        if model_name not in models:
            models[model_name] = files
    write_package(args.output, models)
    summary = ", ".join(models)
    print(f"Merged ESP-SR models: {summary} ({args.output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
