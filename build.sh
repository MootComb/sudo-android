#!/usr/bin/env bash

set -e

usage() {
    echo "Usage: $0 [native|arm|armv7|aarch64|x86]"
    exit 1
}

if [ $# -eq 1 ]; then
    case $1 in
        native)  choice=1 ;;
        arm)     choice=2 ;;
        armv7)   choice=3 ;;
        aarch64) choice=4 ;;
        x86)     choice=5 ;;
        *)
            echo "Invalid argument: $1"
            usage
            ;;
    esac
else
    echo "Select target architecture:"
    echo "  1) native   (gcc)"
    echo "  2) arm      (arm-linux-gnueabi-gcc)     - ARMv5/ARMv6, soft-float"
    echo "  3) armv7    (arm-linux-gnueabihf-gcc)   - ARMv7, hard-float"
    echo "  4) aarch64  (aarch64-linux-gnu-gcc)     - ARM64"
    echo "  5) x86      (i686-linux-gnu-gcc)        - 32-bit x86"
    read -p "Enter choice [1-5]: " choice
fi

case $choice in
    1)
        CC="gcc"
        ;;
    2)
        CC="arm-linux-gnueabi-gcc"
        ;;
    3)
        CC="arm-linux-gnueabihf-gcc"
        ;;
    4)
        CC="aarch64-linux-gnu-gcc"
        ;;
    5)
        CC="i686-linux-gnu-gcc"
        ;;
    *)
        echo "Invalid choice, exiting."
        exit 1
        ;;
esac

echo "Building with $CC..."
make CC="$CC"
