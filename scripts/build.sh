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

echo "Compiling DWOP encoder..."
${CROSS_PREFIX}gcc -O3 -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -DNDEBUG \
    -c src/dsp/dwop_encode.c -o build/dwop_encode.o

echo "Compiling WAV reader..."
${CROSS_PREFIX}gcc -O3 -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -DNDEBUG \
    -c src/dsp/wav_reader.c -o build/wav_reader.o \
    -Isrc/dsp

echo "Compiling REX writer..."
${CROSS_PREFIX}gcc -O3 -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -DNDEBUG \
    -c src/dsp/rex_writer.c -o build/rex_writer.o \
    -Isrc/dsp

echo "Compiling REX plugin..."
${CROSS_PREFIX}gcc -O3 -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -DNDEBUG \
    src/dsp/rex_plugin.c \
    build/dwop.o \
    build/rex_parser.o \
    -o build/dsp.so \
    -Isrc/dsp \
    -lm

echo "Compiling rex-encode CLI..."
${CROSS_PREFIX}gcc -O3 \
    -march=armv8-a -mtune=cortex-a72 \
    -DNDEBUG \
    src/dsp/rex_encode_main.c \
    build/dwop_encode.o \
    build/dwop.o \
    build/wav_reader.o \
    build/rex_writer.o \
    build/rex_parser.o \
    -o build/rex-encode \
    -Isrc/dsp \
    -lm

# Copy files to dist (use cat to avoid ExtFS deallocation issues with Docker)
echo "Packaging..."
cat src/module.json > dist/rex/module.json
[ -f src/help.json ] && cat src/help.json > dist/rex/help.json
cat src/ui.js > dist/rex/ui.js
cat build/dsp.so > dist/rex/dsp.so
chmod +x dist/rex/dsp.so
cat build/rex-encode > dist/rex/rex-encode
chmod +x dist/rex/rex-encode

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
