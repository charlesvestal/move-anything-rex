#!/usr/bin/env bash
# Build REX Player module for Move Anything (ARM64)
#
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== REX Player Module Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building REX Player Module ==="
echo "Cross prefix: $CROSS_PREFIX"

# Create build directories
mkdir -p build
mkdir -p dist/rex

# Compile object files
echo "Compiling DWOP decoder..."
${CROSS_PREFIX}gcc -O3 -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -DNDEBUG \
    -c src/dsp/dwop.c -o build/dwop.o

echo "Compiling REX parser..."
${CROSS_PREFIX}gcc -O3 -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -DNDEBUG \
    -c src/dsp/rex_parser.c -o build/rex_parser.o \
    -Isrc/dsp

echo "Compiling REX plugin..."
${CROSS_PREFIX}gcc -O3 -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -DNDEBUG \
    src/dsp/rex_plugin.c \
    build/dwop.o \
    build/rex_parser.o \
    -o build/dsp.so \
    -Isrc/dsp

# Copy files to dist (use cat to avoid ExtFS deallocation issues with Docker)
echo "Packaging..."
cat src/module.json > dist/rex/module.json
cat src/ui.js > dist/rex/ui.js
cat build/dsp.so > dist/rex/dsp.so
chmod +x dist/rex/dsp.so

# Create loops directory for user-supplied REX files
mkdir -p dist/rex/loops

# Create tarball for release
cd dist
tar -czvf rex-module.tar.gz rex/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/rex/"
echo "Tarball: dist/rex-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
