#!/bin/bash
# build.sh <output folder> <version MMDDYYYY> <revision>
#
# Builds the Debian root filesystem for andr36oid from the official Debian
# archive, writes it as a compressed read-only EROFS image, checks the image
# against the folder it was made from, packs it with xz (cut into parts only if
# one file would be too large for a GitHub release) and writes manifest.json.
#
# Needs root, mmdebstrap, erofs-utils (mkfs.erofs, fsck.erofs), erofs and loop
# support in the kernel, xz, python3 and a C compiler for arm64 (gcc on an
# arm64 machine, or aarch64-linux-gnu-gcc plus qemu-user binfmt support on
# another machine).
#
# Settings from the environment:
#   SUITE       Debian release (default trixie)
#   MIRROR      Debian archive (default http://deb.debian.org/debian)
#   SNAPSHOT    a snapshot.debian.org time stamp such as 20261119T000000Z;
#               when set, packages come from that fixed archive state, which
#               makes the build repeatable later
#   CC          compiler for the gamepad mapper
#   MMDEBSTRAP  mmdebstrap program to use (default: the one in PATH)
#   MAPPER_BIN  an already built a36-padmouse, used instead of compiling
#   KEYRING     the Debian archive keyring file (default: the system's
#               /usr/share/keyrings/debian-archive-keyring.gpg)
set -euo pipefail

out=$1 version=$2 revision=$3
here=$(cd "$(dirname "$0")" && pwd)
case "$version" in [0-1][0-9][0-3][0-9][2-9][0-9][0-9][0-9]) ;; *) echo "version must be MMDDYYYY" >&2; exit 2 ;; esac
case "$revision" in ''|*[!0-9]*|0) echo "revision must be a whole number from 1" >&2; exit 2 ;; esac
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 2; }

SUITE=${SUITE:-trixie}
MIRROR=${MIRROR:-http://deb.debian.org/debian}
SECURITY=${SECURITY:-http://security.debian.org/debian-security}
if [ -n "${SNAPSHOT:-}" ]; then
	MIRROR=https://snapshot.debian.org/archive/debian/$SNAPSHOT
	SECURITY=https://snapshot.debian.org/archive/debian-security/$SNAPSHOT
fi
# The release date is the build date of every file: the same version and
# package set give the same file times.
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(date -u -d "${version:4:4}-${version:0:2}-${version:2:2}" +%s)}
export SOURCE_DATE_EPOCH

# Short on purpose: the console's loop setup allows 64 bytes for the whole
# path, and /mnt/media_rw/XXXX-XXXX/subsystem/darkosen/ already takes 41.
name=debian-$version.img
part_bytes=$((2040 * 1024 * 1024))   # GitHub takes release files under 2 GiB only

mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d "$(dirname "$out")/work.XXXXXX")   # beside the output, never in it
tree=$work/root mnt=$work/mnt loop=
cleanup() {
	mountpoint -q "$mnt" && umount "$mnt"
	[ -z "$loop" ] || losetup -d "$loop" || :
	rm -rf "$work"
}
trap cleanup EXIT

# 1. The gamepad mapper: one small static program, built from this repository.
if [ -n "${MAPPER_BIN:-}" ]; then
	install -m 0755 "$MAPPER_BIN" "$work/a36-padmouse"
else
	if [ -z "${CC:-}" ]; then
		if [ "$(uname -m)" = aarch64 ]; then CC=gcc; else CC=aarch64-linux-gnu-gcc; fi
	fi
	"$CC" -O2 -Wall -static -s -o "$work/a36-padmouse" "$here/padmouse/a36-padmouse.c"
fi

# 2. The root filesystem. On a non-arm64 machine mmdebstrap wants arch-test
# to prove arm64 programs can run; running the static mapper proves the same.
skip=()
# An unpacked mmdebstrap needs its helper mmtarfilter next to it in PATH.
case "${MMDEBSTRAP:-}" in */*) PATH=$(dirname "$MMDEBSTRAP"):$PATH ;; esac
KEYRING=${KEYRING:-/usr/share/keyrings/debian-archive-keyring.gpg}
[ -f "$KEYRING" ] || { echo "no Debian archive keyring at $KEYRING" >&2; exit 1; }
if [ "$(uname -m)" != aarch64 ] && ! command -v arch-test >/dev/null; then
	rc=0; "$work/a36-padmouse" >/dev/null 2>&1 || rc=$?
	[ "$rc" = 2 ] || { echo "this machine cannot run arm64 programs (install qemu-user binfmt support)" >&2; exit 1; }
	skip=(--skip=check/qemu)
fi
packages=$(grep -v '^#' "$here/packages.txt" | grep -v '^$' | paste -sd, -)
export A36_CUSTOMIZE=$here/customize.sh A36_MAPPER=$work/a36-padmouse
"${MMDEBSTRAP:-mmdebstrap}" --mode=root "${skip[@]}" --variant=minbase --architectures=arm64 --format=directory \
	--include="$packages" \
	--aptopt='APT::Install-Recommends "false"' \
	--aptopt='Acquire::Retries "5"' \
	--dpkgopt='path-exclude=/usr/share/man/*' \
	--dpkgopt='path-exclude=/usr/share/info/*' \
	--dpkgopt='path-exclude=/usr/share/locale/*' \
	--dpkgopt='path-exclude=/usr/share/doc/*' \
	--dpkgopt='path-include=/usr/share/doc/*/copyright' \
	--dpkgopt='path-exclude=/usr/share/lintian/*' \
	--customize-hook='"$A36_CUSTOMIZE" "$1" "$A36_MAPPER"' \
	--customize-hook='chroot "$1" dpkg-query -W -f "\${Package}\t\${Version}\t\${Installed-Size}\n" > "$1/../packages.tsv"' \
	"$SUITE" "$tree" \
	"deb [signed-by=$KEYRING] $MIRROR $SUITE main" \
	"deb [signed-by=$KEYRING] $MIRROR $SUITE-updates main" \
	"deb [signed-by=$KEYRING] $SECURITY $SUITE-security main"

# mmdebstrap tidies up after the hooks, so set file times here, last.
find "$tree" -xdev -newermt "@$SOURCE_DATE_EPOCH" -print0 | xargs -0 -r touch -h -d "@$SOURCE_DATE_EPOCH"

# 3. EROFS, with the options the console itself uses, and the stronger lz4hc.
# A fixed UUID per version and revision keeps a rebuild identical.
uuid=$(python3 -c 'import sys, uuid; print(uuid.uuid5(uuid.NAMESPACE_URL, "andr36oid-debian-" + sys.argv[1] + "-r" + sys.argv[2]))' "$version" "$revision")
mkfs.erofs -b4096 -zlz4hc -E^xattr-name-filter --preserve-mtime -U "$uuid" --quiet "$out/$name" "$tree"
fsck.erofs "$out/$name"

# 4. Mount the image read-only and compare it with the folder: kind, mode,
# owner, group, size, time and link target of every entry, every extended
# attribute, and the contents of every file.
mkdir -p "$mnt"
loop=$(losetup --find --show --read-only "$out/$name")
mount -t erofs -o ro "$loop" "$mnt"
listing() {
	(cd "$1" && find . -xdev \( -type d -printf 'd %m %U %G %T@ %p\n' \) -o \
		\( -not -type d -printf '%y %m %U %G %s %T@ %l %p\n' \) | LC_ALL=C sort)
}
attrs() {
	python3 -I -c '
import hashlib, os, sys
root = sys.argv[1]
for top, dirs, files in os.walk(root):
    for entry in [top] + [os.path.join(top, n) for n in dirs + files]:
        for key in sorted(os.listxattr(entry, follow_symlinks=False)):
            value = os.getxattr(entry, key, follow_symlinks=False)
            print(os.path.relpath(entry, root), key, hashlib.sha256(value).hexdigest())
' "$1" | LC_ALL=C sort
}
hashes() { (cd "$1" && find . -xdev -type f -print0 | LC_ALL=C sort -z | xargs -0 -r sha256sum); }
for kind in listing attrs hashes; do
	"$kind" "$tree" > "$work/src.$kind"; "$kind" "$mnt" > "$work/new.$kind"
	cmp -s "$work/src.$kind" "$work/new.$kind" || { diff "$work/src.$kind" "$work/new.$kind" | head -40; echo "image differs from the folder ($kind)" >&2; exit 1; }
done
echo "verified: $(wc -l < "$work/src.listing") entries, $(wc -l < "$work/src.hashes") files identical"
umount "$mnt"; losetup -d "$loop"; loop=

# 5. What the space is used for, for the manifest.
(cd "$tree" && du -sxk ./usr/lib ./usr/share ./usr/bin ./usr/sbin ./usr/libexec ./etc ./var 2>/dev/null) > "$work/usage.tsv"
du -sxk "$tree" | cut -f1 > "$work/total.kb"
mv "$work/packages.tsv" "$work/packages.list"

size=$(stat -c %s "$out/$name")
sum=$(sha256sum "$out/$name" | cut -d' ' -f1)
xz -T0 -6 "$out/$name"
packed_size=$(stat -c %s "$out/$name.xz")
packed_sum=$(sha256sum "$out/$name.xz" | cut -d' ' -f1)
echo "image $size bytes, packed $packed_size bytes"
if [ "$packed_size" -gt "$part_bytes" ]; then
	(cd "$out" && split -b "$part_bytes" -d -a 3 --numeric-suffixes=1 "$name.xz" "$name.xz.")
	rm "$out/$name.xz"
fi

python3 "$here/manifest.py" "$out" "$name" "$size" "$sum" "$version" "$revision" \
	"$packed_size" "$packed_sum" "$work/packages.list" "$work/usage.tsv" "$(cat "$work/total.kb")" "$SUITE"
(cd "$out" && sha256sum -- * > "$work/sums")
mv "$work/sums" "$out/SHA256SUMS"
ls -la "$out"
