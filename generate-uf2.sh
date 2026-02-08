#!/usr/bin/env bash
# Generate .uf2 files from existing .hex files in release-binaries/

set -e  # Exit on error

RELEASE_DIR="release-binaries"

echo "=========================================="
echo "Generating .uf2 files from .hex files"
echo "=========================================="

cd "${RELEASE_DIR}"

# Generate .uf2 for rak4631
if [ -f "rak4631-firmware.hex" ]; then
    echo "Generating rak4631-firmware.uf2..."
    python3 ../bin/uf2conv.py rak4631-firmware.hex -c -o rak4631-firmware.uf2 -f 0xADA52840
    echo "✓ Generated rak4631-firmware.uf2"
else
    echo "⚠ rak4631-firmware.hex not found"
fi

# Generate .uf2 for t-echo
if [ -f "t-echo-firmware.hex" ]; then
    echo "Generating t-echo-firmware.uf2..."
    python3 ../bin/uf2conv.py t-echo-firmware.hex -c -o t-echo-firmware.uf2 -f 0xADA52840
    echo "✓ Generated t-echo-firmware.uf2"
else
    echo "⚠ t-echo-firmware.hex not found"
fi

# Generate .uf2 for Heltec Pocket Qi2 (5Ah)
if [ -f "heltec-mesh-pocket-5000-firmware.hex" ]; then
    echo "Generating heltec-mesh-pocket-5000-firmware.uf2..."
    python3 ../bin/uf2conv.py heltec-mesh-pocket-5000-firmware.hex -c -o heltec-mesh-pocket-5000-firmware.uf2 -f 0xADA52840
    echo "✓ Generated heltec-mesh-pocket-5000-firmware.uf2"
else
    echo "⚠ heltec-mesh-pocket-5000-firmware.hex not found"
fi

# Generate .uf2 for Heltec Pocket Qi2 (10Ah)
if [ -f "heltec-mesh-pocket-10000-firmware.hex" ]; then
    echo "Generating heltec-mesh-pocket-10000-firmware.uf2..."
    python3 ../bin/uf2conv.py heltec-mesh-pocket-10000-firmware.hex -c -o heltec-mesh-pocket-10000-firmware.uf2 -f 0xADA52840
    echo "✓ Generated heltec-mesh-pocket-10000-firmware.uf2"
else
    echo "⚠ heltec-mesh-pocket-10000-firmware.hex not found"
fi

# Regenerate SHA256 checksums
echo ""
echo "Regenerating SHA256 checksums..."
sha256sum * > SHA256SUMS.txt
echo "✓ Updated SHA256SUMS.txt"

cd ..

echo ""
echo "Done! .uf2 files generated in ${RELEASE_DIR}/"
