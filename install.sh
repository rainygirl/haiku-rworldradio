#!/bin/sh
# Builds rworldradio and installs it so it shows up in Deskbar's
# Applications menu. Run this on Haiku, from anywhere:
#   ./install.sh
#
# Deskbar's Applications menu is NOT driven by ~/config/non-packaged/apps/
# (that was tried and confirmed NOT to work) - it's a plain directory of
# symlinks at /boot/system/data/deskbar/menu/Applications/ (see e.g. the
# OpenTTD/Vim entries already there on a real system). That path itself is
# read-only (packagefs), so a non-packaged addition goes in the mirrored
# /boot/system/non-packaged/data/deskbar/menu/Applications/ directory
# instead, which packagefs merges into the read-only view above.
#
# Confirmed on real hardware/QEMU: that merge is NOT picked up live, even
# after restarting Deskbar itself (`quit application/x-vnd.Be-TSKB`) - a
# full reboot was required the first time this directory was created. A
# rebuild/reinstall after that first reboot does not require another one,
# since the directory (and packagefs's merge of it) already exists.
#
# data/ is kept under the separate non-packaged/data/RWorldRadio convention
# (StationCache's own search path, unrelated to the Deskbar menu) rather
# than next to the binary, since StationCache looks there explicitly - see
# README.md.
set -e

cd "$(dirname "$0")"

make

BINARY=$(ls -t objects.*-release/rworldradio 2>/dev/null | head -1)
if [ -z "$BINARY" ]; then
	echo "install.sh: no objects.*-release/rworldradio found after build" >&2
	exit 1
fi

PROJECT_DIR=$PWD
BINARY="$PROJECT_DIR/$BINARY"
DATA_DIR="$PROJECT_DIR/data"
DESKBAR_APPS_DIR=/boot/system/non-packaged/data/deskbar/menu/Applications

mkdir -p "$DESKBAR_APPS_DIR"
mkdir -p ~/config/non-packaged/data

ln -sf "$BINARY" "$DESKBAR_APPS_DIR/RWorldRadio"
ln -sf "$DATA_DIR" ~/config/non-packaged/data/RWorldRadio

echo "Installed:"
echo "  $DESKBAR_APPS_DIR/RWorldRadio -> $BINARY"
echo "  ~/config/non-packaged/data/RWorldRadio -> $DATA_DIR"
echo
if [ -e /boot/system/data/deskbar/menu/Applications/RWorldRadio ]; then
	echo "R World Radio should now appear in Deskbar's Applications menu."
else
	echo "NOTE: if this is the first time installing, you likely need to"
	echo "reboot once before it shows up in Deskbar's Applications menu -"
	echo "the packagefs merge of a newly created non-packaged directory"
	echo "was not observed to happen live, even after restarting Deskbar."
fi
