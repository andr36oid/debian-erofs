#!/bin/bash
# fetch-tools.sh <folder>: download the tools in tools.txt, check their
# SHA-256 and unpack them into <folder>. Installs nothing on the system.
# Prints the path of mmdebstrap.
set -euo pipefail
dest=$1 here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$dest"
grep -v '^#' "$here/tools.txt" | while read -r name url sum; do
	[ -n "$name" ] || continue
	file=$dest/${url##*/}
	[ -f "$file" ] || curl -sSfL --retry 3 -o "$file" "$url"
	echo "$sum  $file" | sha256sum -c --quiet - >&2
	dpkg-deb -x "$file" "$dest/root"
done
echo "$dest/root/usr/bin/mmdebstrap"
