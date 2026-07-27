#!/bin/sh
# Builds rworldradio and installs it so it shows up in Deskbar's
# Applications menu. Run this on Haiku, from anywhere:
#   ./install.sh
#
# Deskbar's Applications menu lists whatever sits directly inside
# ~/config/non-packaged/apps/, so data/ must NOT be linked next to the
# binary there - it would show up as a spurious extra menu entry. It goes
# in the parallel non-packaged/data/RWorldRadio convention instead. See
# README.md's "Installing into the Applications menu" for the manual
# equivalent of what this script does.
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

mkdir -p ~/config/non-packaged/apps
mkdir -p ~/config/non-packaged/data

ln -sf "$BINARY" ~/config/non-packaged/apps/rworldradio
ln -sf "$DATA_DIR" ~/config/non-packaged/data/RWorldRadio

echo "Installed:"
echo "  ~/config/non-packaged/apps/rworldradio -> $BINARY"
echo "  ~/config/non-packaged/data/RWorldRadio -> $DATA_DIR"
echo "R World Radio should now appear in Deskbar's Applications menu."
