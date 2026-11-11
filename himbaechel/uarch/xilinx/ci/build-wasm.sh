#!/bin/bash
# nextpnr-himbaechel with xilinx (the 7-series, on Project X-Ray's database) for WebAssembly (WASI), packaged for npm as
# @silinx/nextpnr-himbaechel-xc7, as spartan3e's ci/build-wasm.sh packages the Spartan-3E's (and YoWASP packages nextpnr,
# github.com/YoWASP/nextpnr: build.sh, npmjs-*): the WASI SDK, Boost and Eigen built for wasm32-wasi, the chip databases
# outside the binary, jco and @yowasp/runtime for JavaScript. Linux only. From the spike of pmnmalo/Silinx#2305
# (branch xc7-spike-2305, spike/build-wasm-xc7.sh); pmnmalo/Silinx#3143.
#   build-wasm.sh <prjxray-db checkout> [work dir]      (run from the repository's root, xilinx's meta submodule checked out)
# Writes <work>/npm: the package (gen/bundle.js, the .wasm, the chip databases gzipped), and its .tgz in <work>/dist.
set -euo pipefail
root=$(pwd)
xraydb=$(cd "$1" && pwd)
work=$(mkdir -p "${2:-wasm-build}" && cd "${2:-wasm-build}" && pwd)
uarch="$root/himbaechel/uarch/xilinx"
# the chip databases: the xc7a50t's (the xc7a35t is its fabric in Project X-Ray's database: the Basys 3, the Arty A7-35)
# and the xc7a100t's (the Nexys A7-100T, the Arty A7-100), as npm/lib/api.js's devices
devices="xc7a50t;xc7a100t"
[ -n "$(ls -A "$uarch/meta" 2>/dev/null)" ] \
  || { echo "xilinx's meta submodule is not checked out (git submodule update --init himbaechel/uarch/xilinx/meta)" >&2; exit 1; }
cd "$work"

# the WASI SDK 34: C++ exceptions as WebAssembly exceptions (nextpnr needs them since it dropped its WASI workarounds,
# 6cf255cc; the SDK's sysroot has them since its version 33, CppExceptions.md). Without LTO: exceptions and LTO do not
# work together yet (WebAssembly/wasi-sdk#629).
WASI_SDK=wasi-sdk-34.0-x86_64-linux
[ -d $WASI_SDK ] || curl -sL https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-34/$WASI_SDK.tar.gz | tar xzf -
WASI_EH="-fwasm-exceptions -mllvm -wasm-use-legacy-eh=false"
WASI_SDK_PATH=$work/$WASI_SDK
BOOST=boost-1.81.0
[ -d $BOOST ] || curl -sL https://github.com/boostorg/boost/releases/download/boost-1.81.0/boost-1.81.0.tar.gz | tar xzf -
EIGEN=eigen-3.4.0
[ -d $EIGEN ] || curl -sL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz | tar xzf -

# single-threaded, as YoWASP's default. libunwind (for -lunwind) is found where the SDK puts it (the sysroot or clang's
# runtime directory, named by the target); the target is wasm32-wasip1 when that is its name, else wasm32-wasi
unwinds=$(find "$WASI_SDK_PATH" -name 'libunwind*.a' 2>/dev/null || true)
echo "libunwind in the WASI SDK: ${unwinds:-none}"
unwind=$(echo "$unwinds" | { grep -v -- '-threads' || true; } | sort | sed -n 1p)
[ -n "$unwind" ] || { echo "no libunwind in the WASI SDK"; exit 1; }
case "$unwind" in *wasip1*) WASI_TARGET=wasm32-wasip1 ;; *) WASI_TARGET=wasm32-wasi ;; esac
echo "WASI target: $WASI_TARGET"
WASI_SYSROOT="--sysroot $WASI_SDK_PATH/share/wasi-sysroot"
# (single-threaded: CMake's Threads is not looked for, else nextpnr asks for boost_thread, which needs threads; on WASI
# pthreads are always there but creating a thread fails)
# the system named WASI, as the SDK's own toolchain does (nextpnr's CMake keys its WASI settings on it), with the
# SDK's platform file
cat > Toolchain-WASI.cmake <<END
cmake_minimum_required(VERSION 3.4...3.31)
list(APPEND CMAKE_MODULE_PATH $WASI_SDK_PATH/share/cmake)
set(CMAKE_SYSTEM_NAME WASI)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR wasm32)
set(CMAKE_C_COMPILER $WASI_SDK_PATH/bin/clang)
set(CMAKE_CXX_COMPILER $WASI_SDK_PATH/bin/clang++)
set(CMAKE_LINKER $WASI_SDK_PATH/bin/wasm-ld CACHE STRING "wasienv build")
set(CMAKE_AR $WASI_SDK_PATH/bin/ar CACHE STRING "wasienv build")
set(CMAKE_RANLIB $WASI_SDK_PATH/bin/ranlib CACHE STRING "wasienv build")
set(CMAKE_C_COMPILER_TARGET $WASI_TARGET)
set(CMAKE_CXX_COMPILER_TARGET $WASI_TARGET)
set(CMAKE_C_FLAGS "$WASI_SYSROOT" CACHE STRING "wasienv build")
set(CMAKE_CXX_FLAGS "$WASI_SYSROOT $WASI_EH" CACHE STRING "wasienv build")
set(CMAKE_EXE_LINKER_FLAGS "$WASI_EH -lunwind -Wl,-z,stack-size=8388608 -Wl,--stack-first -Wl,--strip-all" CACHE STRING "wasienv build")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
END

if [ ! -f $BOOST/stage/lib/libboost_program_options.a ]; then
  [ -f $BOOST/tools/build/src/engine/b2 ] || (cd $BOOST/tools/build/src/engine && ./build.sh > /dev/null)
  cat > $BOOST/project-config.jam <<END
using clang : : clang++ --target=$WASI_TARGET $WASI_SYSROOT $WASI_EH -D_WASI_EMULATED_MMAN -DBOOST_NO_CXX11_HDR_MUTEX ;
project : default-build <toolset>clang ;
libraries = --with-program_options --with-iostreams --with-filesystem --with-system ;
END
  (cd $BOOST && PATH=$WASI_SDK_PATH/bin:$PATH ./tools/build/src/engine/b2 -d0 threading=single link=static stage)
fi
[ -d eigen-prefix ] || { cmake -B eigen-build -S $EIGEN -DCMAKE_INSTALL_PREFIX=$work/eigen-prefix > /dev/null && make -s -C eigen-build install > /dev/null; }

# the native bba assembler (the chip databases' BBA comes from Project X-Ray's database, by gen/xilinx_gen.py: about
# 1-1.5 min and up to 3.5 GB each)
cmake -B bba-build -S "$root/bba" > /dev/null && cmake --build bba-build -j "$(nproc)" > /dev/null

cmake -B nextpnr-build -S "$root" \
  -DCMAKE_TOOLCHAIN_FILE=$work/Toolchain-WASI.cmake -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=$work/$BOOST/stage/ -DBOOST_ROOT=$work/$BOOST -DSTATIC_BUILD=ON \
  -DEigen3_DIR=$work/eigen-prefix/share/eigen3/cmake -DBBA_IMPORT=$work/bba-build/bba-export.cmake \
  -DPython3_EXECUTABLE="$(command -v python3)" -DBUILD_GUI=OFF -DBUILD_PYTHON=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_Threads=ON \
  -DEXTERNAL_CHIPDB=ON -DEXTERNAL_CHIPDB_ROOT=/share \
  -DARCH=himbaechel -DHIMBAECHEL_SPLIT=ON -DHIMBAECHEL_UARCH=xilinx \
  -DHIMBAECHEL_PRJXRAY_DB="$xraydb" "-DHIMBAECHEL_XILINX_DEVICES=$devices" > nextpnr-configure.log 2>&1 || { tail -30 nextpnr-configure.log; exit 1; }
# the chip databases first, two at a time at most (each generator takes up to 3.5 GB)
start=$(date +%s)
cmake --build nextpnr-build -j 2 --target nextpnr-himbaechel-xilinx-chipdb > chipdb-build.log 2>&1 || { tail -30 chipdb-build.log; exit 1; }
echo "chip databases built in $(( $(date +%s) - start )) s"
cmake --build nextpnr-build -j "$(nproc)" > nextpnr-build.log 2>&1 || { grep -E "error" -A3 nextpnr-build.log | head -40; exit 1; }
wasm=$(find nextpnr-build -maxdepth 2 -name 'nextpnr-himbaechel-xilinx*' -type f | sort | sed -n 1p)
chipdbs=$(dirname "$(find nextpnr-build -name 'chipdb-xc7a50t.bin' | sort | sed -n 1p)")
echo "binary: $wasm; chip databases in $chipdbs"
[ -n "$wasm" ] && [ -f "$chipdbs/chipdb-xc7a50t.bin" ] && [ -f "$chipdbs/chipdb-xc7a100t.bin" ] \
  || { find nextpnr-build -maxdepth 3 -newer nextpnr-configure.log -type f | head -30; exit 1; }
ls -la "$wasm" "$chipdbs"

# the package
# the chip databases one file per device (chipdb/chipdb-<device>.bin.gz), fetched by the package's chipdb() only for
# the device in use and given to nextpnr with --chipdb; the runtime's resources hold none (only a note)
rm -rf npm && cp -r "$uarch/npm" npm && mkdir -p npm/share npm/gen npm/chipdb
echo "The chip databases are in ../chipdb, one per device: the package's chipdb(device) and --chipdb." > npm/share/README
for f in "$chipdbs"/chipdb-*.bin; do gzip -9 -c "$f" > "npm/chipdb/$(basename "$f").gz"; done
ls -la npm/chipdb
# the version: package.json's (written there, and in CHANGELOG.md, before the release); a release's tag must be it
# (RELEASE_VERSION, xc7-v0.1.0 -> v0.1.0 -> 0.1.0), else a development one of it (0.1.0-dev.<commits>)
pkgversion=$(node -p 'require("./npm/package.json").version')
rv="${RELEASE_VERSION:-}"; version="${rv#v}"
if [ -n "$version" ] && [ "$version" != "$pkgversion" ]; then
  echo "the tag xc7-$rv is not package.json's version $pkgversion (npm/package.json, npm/CHANGELOG.md)" >&2; exit 1
fi
if [ -n "$version" ] && ! grep -q "^## $version\$" npm/CHANGELOG.md; then
  echo "npm/CHANGELOG.md has no section ## $version" >&2; exit 1
fi
version="${version:-$pkgversion-dev.$(git -C "$root" rev-list --count HEAD)}"
(cd npm && npm pkg set version="$version" > /dev/null && npm install --no-audit --no-fund > /dev/null \
  && npx jco new "$work/$wasm" --wasi-command --output nextpnr-himbaechel-xilinx.wasm \
  && npx jco transpile nextpnr-himbaechel-xilinx.wasm --instantiation async --no-typescript --no-namespaced-exports \
       --map 'wasi:io/*=runtime#io' --map 'wasi:cli/*=runtime#cli' --map 'wasi:clocks/*=runtime#*' \
       --map 'wasi:filesystem/*=runtime#fs' --map 'wasi:random/*=runtime#random' --out-dir gen/ \
  && npx yowasp-pack-resources gen/nextpnr-himbaechel-xilinx-resources.js gen share \
  && npx esbuild --bundle lib/api.js --outfile=gen/bundle.js --format=esm --platform=node \
       --external:./resources-*.js "--define:VERSION=\"$version\"" \
  && mkdir -p "$work/dist" && npm pack --pack-destination "$work/dist")
ls -la "$work/dist"
