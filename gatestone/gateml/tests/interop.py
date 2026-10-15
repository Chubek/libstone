"""Verify D-emitted artifacts with independent, standard-library readers."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import tomllib


class Reader:
    def __init__(self, data):
        self.data = data
        self.offset = 0

    def take(self, count):
        result = self.data[self.offset:self.offset + count]
        assert len(result) == count
        self.offset += count
        return result

    def number(self, width, endian="big", signed=False):
        return int.from_bytes(self.take(width), endian, signed=signed)


def cbor(reader):
    tag = reader.number(1)
    major, extra = tag >> 5, tag & 31
    size = extra if extra < 24 else reader.number(1 << (extra - 24))
    if major == 0:
        return size
    if major == 1:
        return -1 - size
    if major == 3:
        return reader.take(size).decode("utf-8")
    if major == 4:
        return [cbor(reader) for _ in range(size)]
    if major == 5:
        return {cbor(reader): cbor(reader) for _ in range(size)}
    if tag == 0xf6:
        return None
    if tag in (0xf4, 0xf5):
        return tag == 0xf5
    if tag == 0xfb:
        return struct.unpack(">d", size.to_bytes(8, "big"))[0]
    raise AssertionError(f"unexpected CBOR tag: {tag}")


def msgpack(reader):
    tag = reader.number(1)
    if tag < 0x80:
        return tag
    if tag >= 0xe0:
        return tag - 256
    if tag & 0xe0 == 0xa0:
        return reader.take(tag & 31).decode("utf-8")
    if tag & 0xf0 == 0x90:
        return [msgpack(reader) for _ in range(tag & 15)]
    if tag & 0xf0 == 0x80:
        return {msgpack(reader): msgpack(reader) for _ in range(tag & 15)}
    if tag == 0xc0:
        return None
    if tag in (0xc2, 0xc3):
        return tag == 0xc3
    if tag == 0xd3:
        return reader.number(8, signed=True)
    if tag == 0xcb:
        return struct.unpack(">d", reader.take(8))[0]
    raise AssertionError(f"unexpected MessagePack tag: {tag}")


def bson(reader, array=False):
    end = reader.offset + reader.number(4, "little")
    result = {}
    while reader.offset < end - 1:
        tag = reader.number(1)
        key = bytearray()
        while (byte := reader.number(1)) != 0:
            key.append(byte)
        key = key.decode("utf-8")
        if tag == 1:
            value = struct.unpack("<d", reader.take(8))[0]
        elif tag == 2:
            data = reader.take(reader.number(4, "little"))
            assert data[-1] == 0
            value = data[:-1].decode("utf-8")
        elif tag in (3, 4):
            value = bson(reader, array=tag == 4)
        elif tag == 8:
            value = bool(reader.number(1))
        elif tag == 10:
            value = None
        elif tag == 18:
            value = reader.number(8, "little", signed=True)
        else:
            raise AssertionError(f"unexpected BSON tag: {tag}")
        result[key] = value
    assert reader.number(1) == 0 and reader.offset == end
    return [result[str(i)] for i in range(len(result))] if array else result


exe, bundles, build = sys.argv[1:]
with tempfile.TemporaryDirectory(dir=build, prefix="gateml-interop-") as directory:
    subprocess.run([exe, bundles, build, directory], check=True, timeout=50)
    root = Path(directory)
    expected = json.loads((root / "value.json").read_text())
    envelope = tomllib.loads((root / "value.toml").read_text())
    assert json.loads(envelope["gateml_json"]) == expected
    for name, decoder in (("cbor", cbor), ("msgpack", msgpack), ("bson", bson)):
        reader = Reader((root / f"value.{name}").read_bytes())
        assert decoder(reader) == expected, name
        assert reader.offset == len(reader.data)
    data = (root / "value.flatbuffer").read_bytes()
    assert data[4:8] == b"GMLF"
    table = struct.unpack_from("<I", data, 0)[0]
    vtable = table - struct.unpack_from("<i", data, table)[0]
    field = table + struct.unpack_from("<H", data, vtable + 4)[0]
    string = field + struct.unpack_from("<I", data, field)[0]
    length = struct.unpack_from("<I", data, string)[0]
    assert data[string + 4 + length] == 0
    assert json.loads(data[string + 4:string + 4 + length]) == expected
print("GateML independent codec interoperability tests passed")
