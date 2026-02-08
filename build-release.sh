#!/usr/bin/env bash
# Build script for BitChat firmware release binaries

set -e  # Exit on error

VERSION=${1:-"dev"}
RELEASE_DIR="release-binaries"

echo "=========================================="
echo "Building BitChat Firmware v${VERSION}"
echo "=========================================="

# Create release directory
mkdir -p "${RELEASE_DIR}"

# Boards to build (priority order)
BOARDS=(
    "rak4631"                    # nRF52840 - RAK4631 (very popular)
    "tbeam"                     # ESP32 - T-Beam (very popular)
    "heltec-v3"                 # ESP32S3 - Heltec V3 (tested)
    "t-echo"                    # nRF52840 - T-Echo (popular)
    "heltec-mesh-pocket-5000"   # nRF52840 - Heltec Pocket Qi2 (5Ah)
    "heltec-mesh-pocket-10000"  # nRF52840 - Heltec Pocket Qi2 (10Ah)
    "tbeam-s3-core"             # ESP32S3 - T-Beam S3 Core
    "t-deck"                    # ESP32S3 - T-Deck
)

# Track success/failure
SUCCESS=()
FAILED=()

# Build each board
for board in "${BOARDS[@]}"; do
    echo ""
    echo "Building ${board}..."
    echo "----------------------------------------"
    
    if pio run -e "$board" > /tmp/build-${board}.log 2>&1; then
        # Check what file type was created
        if [ -f ".pio/build/${board}/firmware.bin" ]; then
            # ESP32 variant
            cp ".pio/build/${board}/firmware.bin" "${RELEASE_DIR}/${board}-firmware.bin"
            
            # Also copy factory image if it exists (for ESP32)
            if [ -f ".pio/build/${board}/firmware.factory.bin" ]; then
                cp ".pio/build/${board}/firmware.factory.bin" "${RELEASE_DIR}/${board}-firmware.factory.bin"
            fi
            
            echo "✓ ${board} - firmware.bin created"
            SUCCESS+=("${board}")
        elif [ -f ".pio/build/${board}/firmware.hex" ]; then
            # nRF52 variant
            cp ".pio/build/${board}/firmware.hex" "${RELEASE_DIR}/${board}-firmware.hex"
            echo "✓ ${board} - firmware.hex created"
            SUCCESS+=("${board}")
        elif [ -f ".pio/build/${board}/firmware.uf2" ]; then
            # RP2040 variant (if any)
            cp ".pio/build/${board}/firmware.uf2" "${RELEASE_DIR}/${board}-firmware.uf2"
            echo "✓ ${board} - firmware.uf2 created"
            SUCCESS+=("${board}")
        else
            echo "✗ ${board} - No firmware file found!"
            FAILED+=("${board}")
        fi
    else
        echo "✗ ${board} - Build failed!"
        echo "   Check /tmp/build-${board}.log for details"
        FAILED+=("${board}")
    fi
done

echo ""
echo "=========================================="
echo "Build Summary"
echo "=========================================="
echo "Successfully built: ${#SUCCESS[@]} boards"
for board in "${SUCCESS[@]}"; do
    echo "  ✓ ${board}"
done

if [ ${#FAILED[@]} -gt 0 ]; then
    echo ""
    echo "Failed: ${#FAILED[@]} boards"
    for board in "${FAILED[@]}"; do
        echo "  ✗ ${board}"
    done
fi

echo ""
echo "Binaries are in: ${RELEASE_DIR}/"
echo ""

# Generate SHA256 checksums
if [ ${#SUCCESS[@]} -gt 0 ]; then
    echo "Generating SHA256 checksums..."
    cd "${RELEASE_DIR}"
    sha256sum * > SHA256SUMS.txt
    cd ..
    echo "✓ SHA256SUMS.txt created"
fi

echo ""
echo "Done! Ready for GitHub release."
