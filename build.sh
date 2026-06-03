#!/usr/bin/env bash

set -e

echo "Select target architecture:"
echo "  1) native (gcc)"
echo "  2) aarch64 (aarch64-linux-gnu-gcc)"
echo "  3) arm (arm-linux-gnueabihf-gcc)"
read -p "Enter choice [1-3]: " choice

case $choice in
    1)
        CC="gcc"
        ;;
    2)
        CC="aarch64-linux-gnu-gcc"
        ;;
    3)
        CC="arm-linux-gnueabihf-gcc"
        ;;
    *)
        echo "Invalid choice, exiting."
        exit 1
        ;;
esac

echo "Building with $CC..."
make CC="$CC"
