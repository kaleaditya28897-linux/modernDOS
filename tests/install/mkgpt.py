#!/usr/bin/env python3
"""Make a disk image with a GUID partition table, like a UEFI machine's
system disk: an EFI system partition (FAT32, empty) and a second partition
of another type (an "operating system" the installer must not touch).

  mkgpt.py image size_mb esp_mb      prints the ESP's offset in bytes
"""
import struct, sys, uuid, zlib

img, size_mb, esp_mb = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
sectors = size_mb * 2048
esp_first, esp_last = 2048, 2048 + esp_mb * 2048 - 1
os_first, os_last = esp_last + 1, sectors - 34 - 1

def entry(type_guid, first, last, name):
    return (uuid.UUID(type_guid).bytes_le + uuid.uuid4().bytes_le + struct.pack("<QQQ", first, last, 0) +
            name.encode("utf-16-le").ljust(72, b"\0"))

entries = (entry("c12a7328-f81f-11d2-ba4b-00a0c93ec93b", esp_first, esp_last, "EFI System") +
           entry("48465300-0000-11aa-aa11-00306543ecac", os_first, os_last, "Other OS")).ljust(128 * 128, b"\0")

def header(this, other, table):
    h = struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0, this, other, 34, sectors - 34,
                    uuid.uuid4().bytes_le, table, 128, 128, zlib.crc32(entries))
    h = h[:16] + struct.pack("<I", zlib.crc32(h)) + h[20:]
    return h.ljust(512, b"\0")

mbr = bytearray(512)
mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xee, 0xff, 0xff, 0xff, 1, min(sectors - 1, 0xffffffff))
mbr[510:512] = b"\x55\xaa"
with open(img, "wb") as f:
    f.truncate(sectors * 512)
    f.write(mbr)
    f.write(header(1, sectors - 1, 2))
    f.write(entries)
    # the "other OS": recognisable data that must survive
    f.seek(os_first * 512)
    f.write(b"OTHER-OS-DATA" * 1000)
    f.seek((sectors - 33) * 512)
    f.write(entries)
    f.write(header(sectors - 1, 1, sectors - 33))
print(esp_first * 512)
