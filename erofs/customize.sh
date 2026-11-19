#!/bin/bash
# customize.sh <root folder> <a36-padmouse binary>
#
# Run by build.sh through mmdebstrap's customize hook, after all packages are
# installed. Adds the andr36oid files, the desktop user and the service
# settings, then removes what must not be shared between consoles.
set -euo pipefail
root=$1 mapper=$2
here=$(cd "$(dirname "$0")" && pwd)
in_root() { chroot "$root" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin LANG=C.UTF-8 "$@"; }

# Our files, owned by root, modes as in the repository.
cp -a --no-preserve=ownership "$here/overlay/." "$root/"
install -D -m 0755 "$mapper" "$root/usr/libexec/andr36oid-desktop/a36-padmouse"

# The desktop user. Android's numeric groups give access to the games card
# (1023 media_rw, 1015 sdcard_rw), input (1004), audio (1005), network (3003)
# and everybody (9997) once the broker hands those devices over.
# input and render normally come with udev, which a container does not run.
for group in input render; do in_root groupadd -f -r "$group"; done
for pair in a36-media:1023 a36-sdcard:1015 a36-input:1004 a36-audio:1005 a36-inet:3003 a36-everybody:9997; do
	in_root groupadd -f -g "${pair#*:}" "${pair%:*}"
done
in_root useradd -m -u 1000 -U -s /bin/bash \
	-G audio,video,input,render,a36-media,a36-sdcard,a36-input,a36-audio,a36-inet,a36-everybody user
# No passwords in a shared image. Set one from a root shell if you want one.
in_root passwd -l user >/dev/null
in_root passwd -l root >/dev/null

# Background mode: plain multi-user system, no display manager.
in_root systemctl set-default multi-user.target
in_root systemctl disable ssh.service >/dev/null 2>&1 || :
grep -v '^#' "$here/masked-units.txt" | while read -r unit; do
	[ -n "$unit" ] && ln -sf /dev/null "$root/etc/systemd/system/$unit"
done

# Nothing that identifies one console, or one build machine.
rm -f "$root"/etc/ssh/ssh_host_*
: > "$root/etc/machine-id"
rm -f "$root/var/lib/dbus/machine-id"
printf 'debian\n' > "$root/etc/hostname"
: > "$root/etc/fstab"
# Plain Debian sources, whatever mirror the build used.
cat > "$root/etc/apt/sources.list" <<'EOF'
deb http://deb.debian.org/debian trixie main
deb http://deb.debian.org/debian trixie-updates main
deb http://security.debian.org/debian-security trixie-security main
EOF
rm -rf "$root"/var/lib/apt/lists/* "$root"/var/cache/apt/*.bin "$root"/var/cache/apt/archives/*.deb
rm -f "$root"/var/cache/debconf/*-old "$root"/var/lib/dpkg/*-old
find "$root/var/log" -type f -delete
rm -rf "$root"/tmp/* "$root"/var/tmp/*

