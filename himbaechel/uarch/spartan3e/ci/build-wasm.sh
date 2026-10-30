#!/bin/bash
# nextpnr-himbaechel with spartan3e for WebAssembly (WASI), packaged for npm as YoWASP packages nextpnr
# (github.com/YoWASP/nextpnr: build.sh, npmjs-*): the WASI SDK, Boost and Eigen built for wasm32-wasi, the chip
# databases outside the binary (read from /share at run time), jco and @yowasp/runtime for JavaScript. Linux only.
#   build-wasm.sh <silinx checkout> [work dir]      (run from the repository's root)
# Writes <work>/npm: the package (gen/bundle.js, the .wasm, the chip databases as resources), and its .tgz in <work>/dist.
set -euo pipefail
root=$(pwd)
silinx=$(cd "$1" && pwd)
work=$(mkdir -p "${2:-wasm-build}" && cd "${2:-wasm-build}" && pwd)
uarch="$root/himbaechel/uarch/spartan3e"
devices="xc3s100e:cp132 xc3s250e:cp132 xc3s500e:fg320 xc3s1200e:fg320"
cd "$work"

# the WASI SDK 27: C++ exceptions as WebAssembly exceptions (nextpnr needs them since it dropped its WASI workarounds,
# 6cf255cc; the flags of Yosys's own WASI build)
WASI_SDK=wasi-sdk-27.0-x86_64-linux
[ -d $WASI_SDK ] || curl -sL https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-27/$WASI_SDK.tar.gz | tar xzf -
WASI_EH="-fwasm-exceptions -mllvm -wasm-use-legacy-eh=false"
WASI_SDK_PATH=$work/$WASI_SDK
BOOST=boost-1.81.0
[ -d $BOOST ] || curl -sL https://github.com/boostorg/boost/releases/download/boost-1.81.0/boost-1.81.0.tar.gz | tar xzf -
EIGEN=eigen-3.4.0
[ -d $EIGEN ] || curl -sL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz | tar xzf -

# single-threaded, as YoWASP's default
WASI_TARGET=wasm32-wasi
WASI_SYSROOT="--sysroot $WASI_SDK_PATH/share/wasi-sysroot"
cat > Toolchain-WASI.cmake <<END
cmake_minimum_required(VERSION 3.4...3.31)
set(WASI TRUE)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR wasm32)
set(CMAKE_C_COMPILER $WASI_SDK_PATH/bin/clang)
set(CMAKE_CXX_COMPILER $WASI_SDK_PATH/bin/clang++)
set(CMAKE_LINKER $WASI_SDK_PATH/bin/wasm-ld CACHE STRING "wasienv build")
set(CMAKE_AR $WASI_SDK_PATH/bin/ar CACHE STRING "wasienv build")
set(CMAKE_RANLIB $WASI_SDK_PATH/bin/ranlib CACHE STRING "wasienv build")
set(CMAKE_C_COMPILER_TARGET $WASI_TARGET)
set(CMAKE_CXX_COMPILER_TARGET $WASI_TARGET)
set(CMAKE_C_FLAGS "$WASI_SYSROOT -flto" CACHE STRING "wasienv build")
set(CMAKE_CXX_FLAGS "$WASI_SYSROOT -flto $WASI_EH" CACHE STRING "wasienv build")
set(CMAKE_EXE_LINKER_FLAGS "-flto $WASI_EH -lunwind -Wl,-z,stack-size=8388608 -Wl,--stack-first -Wl,--strip-all" CACHE STRING "wasienv build")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
END

if [ ! -f $BOOST/stage/lib/libboost_program_options.a ]; then
  [ -f $BOOST/tools/build/src/engine/b2 ] || (cd $BOOST/tools/build/src/engine && ./build.sh > /dev/null)
  cat > $BOOST/project-config.jam <<END
using clang : : clang++ --target=$WASI_TARGET $WASI_SYSROOT -flto $WASI_EH -D_WASI_EMULATED_MMAN -DBOOST_NO_CXX11_HDR_MUTEX ;
project : default-build <toolset>clang ;
libraries = --with-program_options --with-iostreams --with-filesystem --with-system ;
END
  (cd $BOOST && PATH=$WASI_SDK_PATH/bin:$PATH ./tools/build/src/engine/b2 -d0 threading=single link=static stage)
fi
[ -d eigen-prefix ] || { cmake -B eigen-build -S $EIGEN -DCMAKE_INSTALL_PREFIX=$work/eigen-prefix > /dev/null && make -s -C eigen-build install > /dev/null; }

# the native bba assembler, and the chip database inputs from Silinx
cmake -B bba-build -S "$root/bba" > /dev/null && cmake --build bba-build -j "$(nproc)" > /dev/null
mkdir -p silinx-data
research="$silinx/packages/silinx-fpga/research/nextpnr-s3e"
cmake_devices=""
for d in $devices; do
  part="${d%%:*}${d##*:}-4"
  node "$research/export-unknown-pips.mjs" "$part" "silinx-data/unknown-pips-${d%%:*}.json"
  node "$research/export-timing.mjs" "$part" "silinx-data/timing-${d%%:*}.json"
  cmake_devices="${cmake_devices:+$cmake_devices;}$d"
done

cmake -B nextpnr-build -S "$root" \
  -DCMAKE_TOOLCHAIN_FILE=$work/Toolchain-WASI.cmake -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=$work/$BOOST/stage/ -DBOOST_ROOT=$work/$BOOST -DSTATIC_BUILD=ON \
  -DEigen3_DIR=$work/eigen-prefix/share/eigen3/cmake -DBBA_IMPORT=$work/bba-build/bba-export.cmake \
  -DPython3_EXECUTABLE="$(command -v python3)" -DBUILD_GUI=OFF -DBUILD_PYTHON=OFF \
  -DEXTERNAL_CHIPDB=ON -DEXTERNAL_CHIPDB_ROOT=/share \
  -DARCH=himbaechel -DHIMBAECHEL_SPLIT=ON -DHIMBAECHEL_UARCH=spartan3e \
  -DSILINX_DEVICES="$silinx/packages/silinx-fpga/devices" -DSILINX_UNKNOWN_PIPS=$work/silinx-data -DSILINX_TIMING=$work/silinx-data \
  "-DHIMBAECHEL_SPARTAN3E_DEVICES=$cmake_devices" > nextpnr-configure.log 2>&1 || { tail -30 nextpnr-configure.log; exit 1; }
cmake --build nextpnr-build -j "$(nproc)" > nextpnr-build.log 2>&1 || { grep -E "error" -A3 nextpnr-build.log | head -40; exit 1; }
wasm=$(ls nextpnr-build/nextpnr-himbaechel-spartan3e*.wasm nextpnr-build/nextpnr-himbaechel-spartan3e 2>/dev/null | head -1)
ls -la "$wasm" nextpnr-build/share/himbaechel/spartan3e/

# the package
rm -rf npm && cp -r "$uarch/npm" npm && mkdir -p npm/share/himbaechel/spartan3e npm/gen
cp nextpnr-build/share/himbaechel/spartan3e/chipdb-*.bin npm/share/himbaechel/spartan3e/
version="0.1.0-dev.$(git -C "$root" rev-list --count HEAD)"
(cd npm && npm pkg set version="$version" > /dev/null && npm install --no-audit --no-fund > /dev/null \
  && npx jco new "$work/$wasm" --wasi-command --output nextpnr-himbaechel-spartan3e.wasm \
  && npx jco transpile nextpnr-himbaechel-spartan3e.wasm --instantiation async --no-typescript --no-namespaced-exports \
       --map 'wasi:io/*=runtime#io' --map 'wasi:cli/*=runtime#cli' --map 'wasi:clocks/*=runtime#*' \
       --map 'wasi:filesystem/*=runtime#fs' --map 'wasi:random/*=runtime#random' --out-dir gen/ \
  && npx yowasp-pack-resources gen/nextpnr-himbaechel-spartan3e-resources.js gen share \
  && npx esbuild --bundle lib/api.js --outfile=gen/bundle.js --format=esm --platform=node \
       --external:./resources-*.js "--define:VERSION=\"$version\"" \
  && mkdir -p "$work/dist" && npm pack --pack-destination "$work/dist")
ls -la "$work/dist"
