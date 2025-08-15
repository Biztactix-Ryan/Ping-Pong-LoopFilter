#!/bin/bash

# Clean script for Docker build artifacts
set -e

echo "=== Cleaning Docker build artifacts ==="
echo ""

# Remove build output
if [ -d "build-output" ]; then
    echo "Removing build-output directory..."
    rm -rf build-output
fi

# Remove local build directories if they exist
if [ -d "build_x86_64" ]; then
    echo "Removing build_x86_64 directory..."
    rm -rf build_x86_64
fi

# Optional: Remove Docker images
read -p "Do you want to remove Docker images as well? (y/N) " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Removing Docker images..."
    docker rmi obs-pingpong-builder 2>/dev/null || true
    docker rmi ping-pong-loopfilter_builder 2>/dev/null || true
    docker rmi ping-pong-loopfilter-builder 2>/dev/null || true
fi

echo ""
echo "Clean complete!"