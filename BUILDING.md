# Building Multiflex Core (Quick)

## Linux (native build with depends + CMake)

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config python3 \
  gperf bison flex automake libtool gettext curl git zip unzip

# Build dependencies
make -C depends -j"$(nproc)"

# Find host triplet directory (created by depends build)
HOST_DIR="$(ls -1 depends/*/toolchain.cmake | head -n1 | xargs dirname)"
echo "Using toolchain: ${HOST_DIR}/toolchain.cmake"

# Configure + build
cmake -S . -B build --toolchain "${HOST_DIR}/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(nproc)"

# Install (staging)
cmake --install build --prefix "$PWD/stage" --strip
ls -la stage/bin
