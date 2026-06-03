#!/usr/bin/env bash

set -e

if [ $# -eq 1 ]; then
    case $1 in
        native)  choice=1 ;;
        aarch64) choice=2 ;;
        arm)     choice=3 ;;
        *)
            echo "Invalid argument: $1"
            echo "Usage: $0 [native|aarch64|arm]"
            exit 1
            ;;
    esac
else
    echo "Select target architecture:"
    echo "  1) native (gcc)"
    echo "  2) aarch64 (aarch64-linux-gnu-gcc)"
    echo "  3) arm (arm-linux-gnueabihf-gcc)"
    read -p "Enter choice [1-3]: " choice
fi

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
