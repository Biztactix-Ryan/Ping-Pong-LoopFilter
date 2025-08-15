#!/bin/bash

# Build script for OBS PingPong Loop Filter using Docker
set -e

echo "=== OBS PingPong Loop Filter - Docker Build ==="
echo ""

# OBS version (can be overridden with environment variable)
OBS_VERSION=${OBS_VERSION:-31.1.1}
echo "Building for OBS version: $OBS_VERSION"
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if Docker is installed
if ! command -v docker &> /dev/null; then
    echo -e "${RED}Error: Docker is not installed${NC}"
    echo "Please install Docker from https://docs.docker.com/get-docker/"
    exit 1
fi

# Check if Docker is running
if ! docker info &> /dev/null; then
    echo -e "${RED}Error: Docker is not running${NC}"
    echo "Please start Docker and try again"
    exit 1
fi

# Create build output directory
mkdir -p build-output

echo -e "${YELLOW}Building plugin in Docker container...${NC}"
echo "This may take several minutes on first run as it downloads dependencies."
echo ""

# Build using Docker Compose
if command -v docker-compose &> /dev/null; then
    docker-compose build --build-arg OBS_VERSION=$OBS_VERSION builder
    docker-compose run --rm builder
elif command -v docker &> /dev/null && docker compose version &> /dev/null; then
    docker compose build --build-arg OBS_VERSION=$OBS_VERSION builder
    docker compose run --rm builder
else
    # Fallback to plain Docker
    echo "Building with Docker (without docker-compose)..."
    docker build --build-arg OBS_VERSION=$OBS_VERSION -t obs-pingpong-builder .
    docker run --rm -v "$(pwd)/build-output:/output" obs-pingpong-builder sh -c \
        "cp /plugin/build/*.so /output/ 2>/dev/null || echo 'No .so files found'"
fi

echo ""
echo -e "${GREEN}Build complete!${NC}"
echo ""
echo "Plugin file location:"
ls -la build-output/*.so 2>/dev/null || ls -la build-output/*.dll 2>/dev/null || echo "No plugin files found in build-output/"
echo ""
echo "To install the plugin:"
echo "1. Copy the .so file from build-output/ to your OBS plugins directory"
echo "   - Linux: ~/.config/obs-studio/plugins/obs-pingpong-loop-filter/bin/64bit/"
echo "   - Or system-wide: /usr/lib/obs-plugins/"
echo "2. Restart OBS Studio"