#!/bin/bash
# One-time root steps for boot-ios6 (internet over the USB cable + jailbreak).
#   sudo tools/prep-ios6.sh
# 1. This PC: a NetworkManager profile for the phone's USB Ethernet (host MAC
#    02:00:5e:77:ee:01, 192.168.7.1/24) and a dispatcher hook that shares the
#    PC's connection (NAT) while it is up. Every plug brings it up, unplugging
#    takes it down: nothing to run again.
# 2. Root image v2 = v1 (mkiosroot.sh + sweet-root-tweaks.py) + the Taurine
#    bootstrap (iOS 14) + Taurine's basebinaries (jailbreakd, jbexec,
#    launchjailbreak, pspawn_payload) + jb/debs (installed by dpkg on the first
#    boot) + s9net + root's ssh key + the launchd jobs (tools/jb-launchd.py:
#    jailbreakd, s9net, sshd, one respring per boot) + the default wallpaper
#    (S9Wallpaper tweak) + the Sileo repos.
# Then (as the normal user, phone in TWRP): tools/flash-userdata.sh IMAGE.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
here=$(pwd)
src=../work/iosroot/ios16-root-v1.img
dst=../work/iosroot/ios16-root-v2.img
jb=$here/jb
[ -d "$jb" ] || jb=$here/jailbreak/jb     # repo layout: s9/jailbreak/fetch-jailbreak.sh fills it
fsck=$here/toolchain/bin/fsck.hfsplus

(cd "$jb" && sha256sum -c --quiet SHA256SUMS)

echo "== 1. PC: share the connection with the phone over USB"
nmcli connection delete s9-usb >/dev/null 2>&1 || true
nmcli connection add type ethernet con-name s9-usb ethernet.mac-address 02:00:5E:77:EE:01 \
	connection.autoconnect yes ipv4.method manual ipv4.addresses 192.168.7.1/24 \
	ipv4.never-default yes ipv6.method disabled >/dev/null
cat > /etc/NetworkManager/dispatcher.d/90-ios-s9-share <<'EOF'
#!/bin/sh
# Galaxy S9 iOS over USB (NetworkManager profile s9-usb): NAT this PC's
# connection to the phone (192.168.7.2) while its USB Ethernet is up.
[ "$CONNECTION_ID" = "s9-usb" ] || exit 0
IFACE=$1
rules() {
	iptables -t nat "$1" POSTROUTING -s 192.168.7.0/24 ! -o "$IFACE" -j MASQUERADE
	iptables "$1" FORWARD -i "$IFACE" -j ACCEPT
	iptables "$1" FORWARD -o "$IFACE" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
}
case "$2" in
up)
	sysctl -qw net.ipv4.ip_forward=1
	rules -D 2>/dev/null
	iptables -t nat -A POSTROUTING -s 192.168.7.0/24 ! -o "$IFACE" -j MASQUERADE
	iptables -I FORWARD 1 -i "$IFACE" -j ACCEPT
	iptables -I FORWARD 1 -o "$IFACE" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
	;;
down|pre-down)
	rules -D 2>/dev/null
	;;
esac
exit 0
EOF
chmod 755 /etc/NetworkManager/dispatcher.d/90-ios-s9-share
systemctl enable --now NetworkManager-dispatcher.service >/dev/null 2>&1 || true
echo "profile s9-usb + dispatcher hook installed"

echo "== 2. root image v2"
cp --reflink=auto --sparse=always "$src" "$dst"
loop=$(losetup -f --show "$dst")
mnt=$(mktemp -d)
cleanup() {
	mountpoint -q "$mnt" && umount "$mnt"
	losetup -d "$loop" 2>/dev/null || true
	rmdir "$mnt" 2>/dev/null || true
}
trap cleanup EXIT
"$fsck" -fy "$loop" >/dev/null || echo "fsck: exit $? (repaired or warnings)"
mount -t hfsplus -o rw,force "$loop" "$mnt"

# Taurine's bootstrap (iOS 14, iphoneos-arm): /usr, /Library/dpkg, apt, dpkg,
# bash/zsh. This is what the Taurine app unpacks to / on a real device; we do
# it here so the image boots with a working package manager already in place.
# Note it ships as .tar.gz that is really zstd, so let tar detect it.
tar -xpf "$jb/bootstrap.tar.gz" --numeric-owner --keep-directory-symlink -C "$mnt"

# libhooker + safemode + Sileo's files in place from the first boot (dpkg
# registers them later). libhooker is the iOS 14 injection layer; ElleKit does
# not exist for 14.3. Each deb's data.tar is xz or zst depending on the build,
# so extract with whatever the member name says.
for d in "$jb/debs"/*.deb; do
	case "$d" in *sileo*|*libhooker*|*safemode*|*preferenceloader*)
		m=$(ar t "$d" | grep -E '^data\.tar') || continue
		case "$m" in
		*.zst) ar p "$d" "$m" | zstd -dc | tar -xp --numeric-owner --keep-directory-symlink -C "$mnt" ;;
		*.xz)  ar p "$d" "$m" | tar -xJp --numeric-owner --keep-directory-symlink -C "$mnt" ;;
		*.gz)  ar p "$d" "$m" | tar -xzp --numeric-owner --keep-directory-symlink -C "$mnt" ;;
		*)     ar p "$d" "$m" | tar -xp --numeric-owner --keep-directory-symlink -C "$mnt" ;;
		esac
		;;
	esac
done

# Taurine's basebinaries: jailbreakd is what injects; jbexec and
# launchjailbreak are its helpers, pspawn_payload.dylib is the spawn hook.
install -d -m 755 "$mnt/taurine"
tar -xpf "$jb/basebinaries.tar" --numeric-owner --keep-directory-symlink -C "$mnt/taurine"
chmod 755 "$mnt/taurine/jailbreakd" "$mnt/taurine/jbexec" "$mnt/taurine/launchjailbreak"
chmod 755 "$mnt/taurine/amfidebilitate" 2>/dev/null || true
chmod 644 "$mnt/taurine"/*.dylib 2>/dev/null || true

install -d -m 755 "$mnt/private/var/ios-s9/debs"
install -m 644 "$jb"/debs/*.deb "$mnt/private/var/ios-s9/debs/"
install -m 755 "$jb/s9net" "$mnt/usr/libexec/s9net"
install -d -m 700 "$mnt/private/var/root/.ssh"
install -m 600 "$jb/authorized_keys" "$mnt/private/var/root/.ssh/authorized_keys"
chown -R 0:0 "$mnt/private/var/root/.ssh"

cat > "$mnt/usr/libexec/ios-s9-jbsetup" <<'EOF'
#!/bin/sh
# Galaxy S9 iOS: first-boot jailbreak setup (launchd job com.ios-s9.jbsetup, root).
exec >>/private/var/ios-s9/jbsetup.log 2>&1
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
[ -f /private/var/ios-s9/jbsetup.done ] && exit 0
echo "=== $(date) jbsetup start"
sleep 60        # let SpringBoard, lsd and installd come up for uicache
if [ -f /prep_bootstrap.sh ]; then
	NO_PASSWORD_PROMPT=1 /bin/sh /prep_bootstrap.sh
	printf 'alpine\n' | /usr/sbin/pw usermod 501 -h 0
	printf 'alpine\n' | /usr/sbin/pw usermod 0 -h 0
fi
# libhooker is Taurine's injection library and expects jailbreakd to be running
# (started by com.ios-s9.jbstart). Its postinst must NOT be skipped, so leave
# /.installed_taurine absent.
dpkg -i /private/var/ios-s9/debs/*.deb || dpkg -i --force-depends /private/var/ios-s9/debs/*.deb
dpkg --configure -a
uicache -a
touch /private/var/ios-s9/jbsetup.done
echo "=== $(date) jbsetup done, respring"
sbreload || killall -9 SpringBoard
EOF
chmod 755 "$mnt/usr/libexec/ios-s9-jbsetup"

# One respring per boot (launchd job com.ios-s9.respring, tools/jb-launchd.py):
# SpringBoard starts before jailbreakd has set up injection, so the first
# SpringBoard of every boot has no tweaks.
cat > "$mnt/usr/libexec/ios-s9-respring" <<'EOF'
#!/bin/sh
# Galaxy S9 iOS: one respring per boot (launchd job com.ios-s9.respring).
# Tweak injection comes from libhooker through jailbreakd (started by
# com.ios-s9.jbstart). SpringBoard is spawned by launchd before that is ready,
# so the first SpringBoard of every boot runs without tweaks. Wait for
# SpringBoard and jailbreakd, then restart SpringBoard once.
running() { ps -axo comm | grep -q "$1"; }
i=0
while [ $i -lt 900 ]; do
	if running '/SpringBoard$' && running 'jailbreakd$'; then
		break
	fi
	sleep 2
	i=$((i + 2))
done
sleep 20
echo "$(date) respring after ${i}s"
killall -9 SpringBoard
EOF
chmod 755 "$mnt/usr/libexec/ios-s9-respring"

# Default wallpaper: PosterBoard has no poster providers on this root, so the
# S9Wallpaper tweak (s9/ios-tools) draws jb/wallpaper.jpg on the lock screen.
# Into usr/lib/TweakInject directly: /Library/MobileSubstrate/DynamicLibraries
# is an absolute symlink to it in the Taurine/libhooker bootstrap, which would
# resolve against this PC instead of the image if we wrote through it.
if [ -f "$jb/S9Wallpaper.dylib" ] && [ -f "$jb/wallpaper.jpg" ]; then
	install -d -m 755 "$mnt/usr/lib/TweakInject"
	install -m 755 "$jb/S9Wallpaper.dylib" "$mnt/usr/lib/TweakInject/S9Wallpaper.dylib"
	install -m 644 "$jb/S9Wallpaper.plist" "$mnt/usr/lib/TweakInject/S9Wallpaper.plist"
	install -d -m 755 -o 501 -g 501 "$mnt/private/var/mobile/Library/S9Wallpaper"
	install -m 644 -o 501 -g 501 "$jb/wallpaper.jpg" "$mnt/private/var/mobile/Library/S9Wallpaper/wallpaper.jpg"
else
	echo "no jb/S9Wallpaper.dylib or jb/wallpaper.jpg: default wallpaper skipped (s9/jailbreak/fetch-jailbreak.sh)"
fi

install -d -m 755 "$mnt/private/etc/apt/sources.list.d"
# Sileo/apt repos: each answered with a valid Release + Packages index on
# 2026-10-24 (jailbreak/repos.sources in the repo, jb/repos.sources locally).
if [ -f "$here/jailbreak/repos.sources" ]; then
	install -m 644 "$here/jailbreak/repos.sources" "$mnt/private/etc/apt/sources.list.d/ios-s9.sources"
else
	install -m 644 "$jb/repos.sources" "$mnt/private/etc/apt/sources.list.d/ios-s9.sources"
fi

python3 -I "$here/tools/jb-launchd.py" "$mnt"
python3 -I "$here/tools/trim-daemons.py" "$mnt/System/Library/xpc/launchd.plist" --apply
# No Bluetooth hardware: mediaserverd's BTAudioHALPlugin looked bluetoothd up over
# XPC forever (launchd ~70 % + mediaserverd ~50 % of a core at idle).
if [ -d "$mnt/System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver" ]; then
	mkdir -p "$mnt/Library/Audio/Plug-Ins/HAL.disabled"
	mv "$mnt/System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver" "$mnt/Library/Audio/Plug-Ins/HAL.disabled/"
fi
sync
umount "$mnt"
"$fsck" -fy "$loop" >/dev/null || echo "fsck: exit $? (repaired or warnings)"
losetup -d "$loop"
trap - EXIT
rmdir "$mnt"
chown "${SUDO_UID:-0}:${SUDO_GID:-0}" "$dst"
echo "image: $dst"
echo "next (normal user, phone in TWRP): tools/flash-userdata.sh $dst"
