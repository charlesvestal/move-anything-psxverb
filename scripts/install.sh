#!/bin/bash
# Install PSX Verb module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/psxverb" ]; then
    echo "Error: dist/psxverb not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing PSX Verb Module ==="

# Deploy to Move (audio_fx path)
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/move-anything/modules/chain/audio_fx/psxverb"
scp -r dist/psxverb/* ableton@move.local:/data/UserData/move-anything/modules/chain/audio_fx/psxverb/

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/move-anything/modules/chain/audio_fx/psxverb/"
echo ""
echo "Restart Move Anything to load the new module."
