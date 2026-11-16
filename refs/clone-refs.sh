#!/bin/bash
# Clone the reference sources listed in REFS.md.
#
# Nothing here is redistributed by this repository -- this fetches each project
# from its own upstream. See REFS.md for what each one teaches and its license.
#
#   ./clone-refs.sh              everything (~3.5 GB)
#   ./clone-refs.sh --small      only the ones under 10 MB (~60 MB)
#   ./clone-refs.sh ios-flame    one or more by name
#   ./clone-refs.sh --list       show the names
#
# Clones land in refs/src/<name>/, which is gitignored.
set -euo pipefail
cd "$(dirname "$0")"
DEST=src
mkdir -p "$DEST"

# name|url|size-class|note
#   size-class: small (<10 MB) or large
REFS='
ios-sweet|https://github.com/dipa-studio/ios-sweet.git|small|the origin of this family; MIT
ios-flame|https://github.com/NWylynko/ios-flame.git|small|Pixel 4, Metal working; the ppnum_t fix
ios-sailfish|https://github.com/devilAPI/ios-sailfish.git|small|Pixel 1, GPU bring-up in order (fork of dipa-studio; branch axon7)
ios-topaz|https://github.com/MakrSas/ios-topaz.git|small|Redmi Note 12; proving the display path
ios-apollo|https://github.com/enesuzun2002/hakios-apollo.git|small|Galaxy S20; zap shader via TrustZone
Chlorophyll|https://github.com/ry-diffusion/Chlorophyll.git|small|AIR to NIR; Metal without Apple
iOSPlus10|https://github.com/MellysInformejtik/slop.git|small|arm64e and PAC
Portsmith-iOS|https://github.com/Chromitsune/Portsmith-iOS.git|large|catalogue of 112 ports; MIT
veerhooda-iOS|https://github.com/Veerhooda/iOS.git|small|iOS 12 on OnePlus 2; op2hfs.py
veerhooda-xnu|https://github.com/Veerhooda/xnu.git|large|its kernel, branch msm8994
Bergamota|https://github.com/ry-diffusion/Bergamota.git|large|iOS 18 on SDM845; NO LICENSE, read only
metal2vulkan|https://github.com/steelbrain/metal2vulkan.git|large|Metal on top of Vulkan
hakios-os|https://github.com/oosexgang/hackios-xnu-kernel.git|large|QEMU and fastboot boot
Taurine|https://github.com/Odyssey-Team/Taurine.git|large|the iOS 14 jailbreak; rootful
xnu-7195|https://github.com/apple-oss-distributions/xnu.git|large|the kernel we run (check out xnu-7195)
'

list() { echo "$REFS" | sed '/^$/d' | cut -d'|' -f1; }

if [ "${1:-}" = "--list" ]; then list; exit 0; fi

MODE=all
WANT=()
case "${1:-}" in
	--small) MODE=small ;;
	"")      ;;
	*)       WANT=("$@") ;;
esac

clone_one() {
	name=$1 url=$2 note=$3
	if [ -d "$DEST/$name/.git" ]; then
		echo "  = $name (already cloned)"
		return
	fi
	echo "  + $name -- $note"
	git clone --depth 1 --quiet "$url" "$DEST/$name" || {
		echo "    FAILED: $url" >&2
		return 1
	}
}

echo "$REFS" | sed '/^$/d' | while IFS='|' read -r name url size note; do
	[ -z "$name" ] && continue
	if [ ${#WANT[@]} -gt 0 ]; then
		hit=0
		for w in "${WANT[@]}"; do [ "$w" = "$name" ] && hit=1; done
		[ $hit -eq 0 ] && continue
	elif [ "$MODE" = small ] && [ "$size" != small ]; then
		continue
	fi
	clone_one "$name" "$url" "$note"
done

# xnu needs a specific tag, not the default branch
if [ -d "$DEST/xnu-7195/.git" ] && [ "$(git -C "$DEST/xnu-7195" describe --tags 2>/dev/null | head -c 9)" != "xnu-7195." ]; then
	echo "  ~ xnu-7195: checking out the 7195 tag"
	git -C "$DEST/xnu-7195" fetch --depth 1 --quiet origin tag xnu-7195.141.2 2>/dev/null || true
	git -C "$DEST/xnu-7195" checkout --quiet xnu-7195.141.2 2>/dev/null || \
		echo "    (tag not found; the default branch is fine for reading)"
fi

echo
echo "Done. Read REFS.md for what each one teaches."
echo "Note: Bergamota, ios-topaz, ios-sailfish and hakios-sm8150 ship no license."
echo "      Read them; do not vendor them into your own repository."
