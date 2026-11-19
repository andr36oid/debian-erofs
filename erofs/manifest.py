#!/usr/bin/env python3
"""manifest.py <out> <image name> <size> <sha256> <version> <revision> <packed size>
              <packed sha256> <package list> <usage list> <unpacked KiB> <suite>

Writes manifest.json, the one file an installer reads to know what this release
is: the version, how large the EROFS image is, the file(s) to download and how
large a partition should be made for it. It also lists every Debian package in
the image and what the space is used for.

The field names are the ones the andr36oid installer already reads for the
dArkOS images. "darkosen" is that installer's name for the release version
(MMDDYYYY); "distribution" says this is the Debian image.
"""
import hashlib, json, os, sys

(out, name, size, digest, version, revision, packed_size, packed_digest,
 package_list, usage_list, unpacked_kib, suite) = sys.argv[1:13]
size, packed_size = int(size), int(packed_size)
GIB = 1 << 30


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


# One file <name>.xz, or <name>.xz.001, .002, ... to be joined in that order
parts = []
for part in sorted(p for p in os.listdir(out) if p.startswith(name + ".xz")):
    path = os.path.join(out, part)
    parts.append({"name": part, "size": os.path.getsize(path), "sha256": sha256(path)})
if sum(p["size"] for p in parts) != packed_size:
    sys.exit("download files do not add up to the packed size")

packages = []
with open(package_list) as f:
    for line in f:
        fields = line.rstrip("\n").split("\t")
        if len(fields) == 3 and fields[0]:
            packages.append({"name": fields[0], "version": fields[1],
                             "installed_kib": int(fields[2] or 0)})
packages.sort(key=lambda p: p["name"])

usage = {}
with open(usage_list) as f:
    for line in f:
        kib, path = line.rstrip("\n").split("\t", 1)
        usage[path.lstrip(".")] = int(kib) * 1024

minimum = ""
try:
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "minimum-andr36oid.txt")) as f:
        minimum = f.read().strip()
except OSError:
    pass

manifest = {
    "format": 1,
    "distribution": "debian",
    "darkosen": version,
    "revision": int(revision),
    "debian": {"suite": suite, "architecture": "arm64"},
    # the image as it goes onto the card, after unpacking
    "image": {"name": name, "filesystem": "erofs", "compression": "lz4hc",
              "size": size, "sha256": digest},
    "download": {"compression": "xz", "name": name + ".xz", "size": packed_size,
                 "sha256": packed_digest, "parts": parts},
    # room for a later, somewhat larger release: a quarter on top, whole GiB
    "partition_bytes": -(-(size + size // 4) // GIB) * GIB,
    "minimum_andr36oid": minimum,
    "hotfixes": [],
    "contents": {"unpacked_bytes": int(unpacked_kib) * 1024, "by_folder": usage,
                 "largest_packages": [p["name"] for p in sorted(packages, key=lambda p: -p["installed_kib"])[:15]]},
    "packages": packages,
}
with open(os.path.join(out, "manifest.json"), "w") as f:
    json.dump(manifest, f, indent=1)
    f.write("\n")
if os.path.getsize(os.path.join(out, "manifest.json")) > 1 << 20:
    sys.exit("manifest.json is over 1 MiB; the installer would not read it")
print(json.dumps({k: v for k, v in manifest.items() if k != "packages"}, indent=2))
