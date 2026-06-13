#!/bin/bash
# Double-clickable launcher for the HEIC to JPG Converter.
cd "$(dirname "$0")"

echo "============================================"
echo "  HEIC to JPG Converter"
echo "============================================"
echo

# Remove the macOS "quarantine" flag so the unsigned program can run.
xattr -d com.apple.quarantine ./heic_converter_mt 2>/dev/null

echo "Converting every .heic photo in the"
echo "\"photos to convert\" folder..."
echo
./heic_converter_mt "photos to convert"
echo
echo "Done. Your .jpg files are in the \"output\" folder."
echo
echo "Press any key to close this window."
read -n 1 -s
