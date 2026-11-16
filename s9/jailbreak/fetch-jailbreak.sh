#!/bin/bash
# Download the third-party jailbreak pieces for the Galaxy S9 iOS 14.3 root image.
#
# This repository does NOT redistribute these binaries -- they belong to their
# authors under their own licenses (see CREDITS). This script fetches them from
# their official sources into ./jb/, which tools/prep-ios6.sh then installs into
# the iOS root.
#
# WHY TAURINE AND NOT PALERA1N
#
# The iOS 16 tree used palera1n's Procursus bootstrap (CFVER 1800). That does
# not exist for iOS 14: apt.procurs.us only publishes 1800/1900/2000/3000, and
# static.palera.in has bootstrap-1700 and -1800 but no -1400. There is no
# Procursus bootstrap for 14.3 to download, at any URL.
#
# Taurine (Odyssey Team) is the jailbreak for exactly this target: iOS 14.0-14.3,
# A8-A14. It ships its own bootstrap inside the app (Taurine/resources/
# bootstrap.tar.gz, 13 MB, 2908 files, iphoneos-arm) instead of depending on
# Procursus, and its injection layer is libhooker, not ElleKit -- ElleKit starts
# at iOS 15. The basebinaries (jailbreakd, jbexec, launchjailbreak,
# pspawn_payload.dylib) are what actually inject.
#
# Architecture: iphoneos-arm (arm64). The iPhone XR 14.3 userland is arm64, not
# arm64e, so the arm64 slices are the ones that matter.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p jb/debs

TAURINE_RAW=https://raw.githubusercontent.com/Odyssey-Team/Taurine/main/Taurine/resources
ODYSSEY_REPO=https://repo.theodyssey.dev

echo "== Taurine bootstrap (iOS 14, iphoneos-arm) =="
# The bootstrap the Taurine app unpacks to / on a real device. We unpack it
# ourselves at image-build time instead, so the root is ready before first boot.
curl -fsSL -o jb/bootstrap.tar.gz "$TAURINE_RAW/bootstrap.tar.gz"

echo "== Taurine basebinaries (jailbreakd, jbexec, launchjailbreak, pspawn) =="
# These are what inject. On a real device the Taurine app installs them to
# /taurine and starts jailbreakd; here they go into the image and the launchd
# job com.ios-s9.jbsetup starts them.
curl -fsSL -o jb/basebinaries.tar "$TAURINE_RAW/basebinaries.tar"

echo "== core packages =="
dl() { echo "  $1"; curl -fsSL -o "jb/debs/$2" "$1"; }

# Sileo 2.3 is the build Taurine itself ships (resources/org.coolstar.sileo_2.3).
# 2.5.1 is the iOS 16 one and is not used here.
dl "$TAURINE_RAW/org.coolstar.sileo_2.3_iphoneos-arm.deb" 0-sileo.deb

# Sileo is a Swift app and links libswiftCore/Foundation/UIKit/... HARD (not
# weak). iOS 14.3 ships only libswiftDemangle and libswiftXCTest in
# /usr/lib/swift, so without this package Sileo dies at launch with a dyld
# "Library not loaded" error. This is the Swift 5.0 runtime, built for
# iOS 7.0+, installed to /usr/lib/libswift/stable -- which is already on
# Sileo's LC_RPATH, so no symlinks are needed.
dl "$ODYSSEY_REPO/debs/org.swift.libswift_5.0-electra2_iphoneos-arm.deb" 8-libswift.deb

# libhooker is the injection library on iOS 14. The -newabi variant is the one
# that declares firmware (>= 14.0); the plain package pulls it in as a
# dependency, but we fetch it explicitly so the image is complete offline.
dl "$ODYSSEY_REPO/debs/org.coolstar.libhooker_1.6.9-1_iphoneos-arm.deb"      1-libhooker.deb
dl "$ODYSSEY_REPO/debs/org.coolstar.libhooker-newabi_1.6.8_iphoneos-arm.deb" 2-libhooker-newabi.deb
dl "$ODYSSEY_REPO/debs/org.coolstar.safemode_1.1.4_iphoneos-arm.deb"         3-safemode.deb
dl "$ODYSSEY_REPO/debs/preferenceloader_3.0.3_iphoneos-arm.deb"              4-preferenceloader.deb

# NewTerm 3 and OpenSSH come from the Procursus 1700 pool: they are plain
# command-line tools with no firmware ceiling, and 1700 is the oldest suite
# Procursus still publishes. They are NOT the bootstrap -- the bootstrap is
# Taurine's.
dl "https://apt.procurs.us/pool/main/iphoneos-arm64/1700/openssh/openssh-server_9.7p1-1_iphoneos-arm.deb" 5-openssh-server.deb
dl "https://apt.procurs.us/pool/main/iphoneos-arm64/1700/openssh/openssh-client_9.7p1-1_iphoneos-arm.deb" 6-openssh-client.deb
# NewTerm 2.5, NOT 3.0. NewTerm 3 is unusable here for two independent reasons:
# it links libswiftUniformTypeIdentifiers.dylib HARD (that dylib does not exist
# on 14.3 -- UniformTypeIdentifiers arrived in 14.5 and the framework here has
# only Info.plist), and it is the ROOTLESS build (spawns /var/jb/usr/bin/login;
# a rootful jailbreak has no /var/jb).
#
# NewTerm 2.5 is the right one: LC_VERSION_MIN_IPHONEOS 10.0, it spawns
# /usr/bin/login (rootful, present in the rootfs), and every Swift dylib it
# links HARD is in org.swift.libswift. Its Swift deps are also mostly weak.
dl "https://repo.chariz.com/debs/ws.hbang.newterm2_2.5_iphoneos-arm.deb" 7-newterm.deb

# Cephei (ws.hbang.common) is NewTerm 2's UI framework. It declares
# mobilesubstrate, which libhooker Provides (= 99.4), and it links
# CydiaSubstrate, which libhooker ships as
# /Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate.
dl "https://repo.chariz.com/debs/ws.hbang.common_1.16_iphoneos-arm.deb" 9-cephei.deb

echo "== ios-s9's own iOS pieces (this repo's release; source in s9/ios-tools) =="
REL=${S9_RELEASE:-https://github.com/syncmodz/ios-s9/releases/download/s9-2026.10.24}
for f in s9net S9Wallpaper.dylib S9Wallpaper.plist; do
  echo "  $f"; curl -fsSL -o "jb/$f" "$REL/$f"
done
chmod 755 jb/s9net
# default wallpaper (original artwork, wallpaper/make-wallpaper.py)
cp wallpaper/wallpaper.jpg jb/wallpaper.jpg

echo "== your SSH key (for root@192.168.7.2 over the USB cable) =="
[ -f jb/authorized_keys ] || cp "${HOME}/.ssh/ios-s9.pub" jb/authorized_keys 2>/dev/null || \
  echo "  (put your public key at jb/authorized_keys)"

# tools/prep-ios6.sh checks what was fetched against this list
(cd jb && find . -type f ! -name SHA256SUMS ! -name authorized_keys | sort | xargs sha256sum > SHA256SUMS)

echo
echo "Fetched. Next: sudo tools/prep-ios6.sh (see its header for the paths)."
