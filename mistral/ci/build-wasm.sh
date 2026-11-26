#!/bin/bash
# nextpnr-mistral (the Cyclone V, on Mistral's database) for WebAssembly (WASI), packaged for npm as @silinx/nextpnr-mistral,
# as himbaechel/uarch/xilinx/ci/build-wasm.sh packages the 7-series' (and YoWASP packages nextpnr, github.com/YoWASP/nextpnr):
# the WASI SDK, Boost, Eigen and liblzma built for wasm32-wasi, jco and @yowasp/runtime for JavaScript. Mistral's routing
# data are files beside the binary (mistral/ci/mistral-external-data.mjs): global.bin in the package's resources
# (/share/mistral), each die's <die>-r.bin a file of the package, fetched for the device in use and given to nextpnr in its
# working directory. pmnmalo/Silinx#5377. Linux only.
#   build-wasm.sh <mistral checkout> [work dir]      (run from the repository's root)
# Writes <work>/npm: the package (gen/bundle.js, the .wasm, data/<die>-r.bin), and its .tgz in <work>/dist.
set -euo pipefail
root=$(pwd)
mistral_src=$(cd "$1" && pwd)
work=$(mkdir -p "${2:-wasm-build}" && cd "${2:-wasm-build}" && pwd)
arch="$root/mistral"
# the dies whose data the package has: the e50f (the Cyclone V E A2 / A4: the DE0-CV's 5CEBA4) and the sx120f (the
# Cyclone V SE A5 / A6: the DE1-SoC's 5CSEMA5 and the DE10-Nano's 5CSEBA6), as npm/lib/api.js's dies
dies="e50f sx120f"
cd "$work"

# the versions, pinned (the workflow's MISTRAL_REF is Mistral's commit)
# pinned: {"kind": "tag", "name": "WASI SDK (nextpnr-mistral's WebAssembly build)", "repo": "WebAssembly/wasi-sdk", "match": "WASI_SDK_TAG=(?<tag>wasi-sdk-\\d+)"}
WASI_SDK_TAG=wasi-sdk-34
WASI_SDK=wasi-sdk-34.0-x86_64-linux
# pinned: {"kind": "tag", "name": "xz (liblzma, Mistral's decompression)", "repo": "tukaani-project/xz", "match": "XZ=xz-(?<tag>[\\d.]+)"}
XZ=xz-5.8.1
XZ_SHA256=507825b599356c10dca1cd720c9d0d0c9d5400b9de300af00e4d1ea150795543
BOOST=boost-1.81.0
EIGEN=eigen-3.4.0

# the WASI SDK 34: C++ exceptions as WebAssembly exceptions (nextpnr needs them, 6cf255cc); without LTO
# (WebAssembly/wasi-sdk#629). As the 7-series' build.
[ -d $WASI_SDK ] || curl -sL https://github.com/WebAssembly/wasi-sdk/releases/download/$WASI_SDK_TAG/$WASI_SDK.tar.gz | tar xzf -
WASI_EH="-fwasm-exceptions -mllvm -wasm-use-legacy-eh=false"
WASI_SDK_PATH=$work/$WASI_SDK
[ -d $BOOST ] || curl -sL https://github.com/boostorg/boost/releases/download/$BOOST/$BOOST.tar.gz | tar xzf -
[ -d $EIGEN ] || curl -sL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/$EIGEN.tar.gz | tar xzf -
if [ ! -d $XZ ]; then
  curl -sL -o $XZ.tar.gz https://github.com/tukaani-project/xz/releases/download/v${XZ#xz-}/$XZ.tar.gz
  echo "$XZ_SHA256  $XZ.tar.gz" | sha256sum -c --quiet - && tar xzf $XZ.tar.gz && rm $XZ.tar.gz
fi

unwind=$(find "$WASI_SDK_PATH" -name 'libunwind*.a' 2>/dev/null | { grep -v -- '-threads' || true; } | sort | sed -n 1p)
[ -n "$unwind" ] || { echo "no libunwind in the WASI SDK"; exit 1; }
case "$unwind" in *wasip1*) WASI_TARGET=wasm32-wasip1 ;; *) WASI_TARGET=wasm32-wasi ;; esac
echo "WASI target: $WASI_TARGET"
WASI_SYSROOT="--sysroot $WASI_SDK_PATH/share/wasi-sysroot"
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
# liblzma, single-threaded, the library only
if [ ! -f xz-prefix/lib/liblzma.a ]; then
  cmake -B xz-build -S $XZ -DCMAKE_TOOLCHAIN_FILE=$work/Toolchain-WASI.cmake -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=$work/xz-prefix -DBUILD_SHARED_LIBS=OFF -DXZ_THREADS=no -DXZ_NLS=OFF -DXZ_SANDBOX=no \
    -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF -DXZ_TOOL_LZMADEC=OFF -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_SCRIPTS=OFF \
    -DXZ_DOC=OFF -DXZ_MICROLZMA_ENCODER=OFF -DXZ_MICROLZMA_DECODER=OFF -DXZ_LZIP_DECODER=OFF \
    > xz-configure.log 2>&1 || { tail -30 xz-configure.log; exit 1; }
  cmake --build xz-build -j "$(nproc)" --target liblzma > xz-build.log 2>&1 || { grep -E "error" -A3 xz-build.log | head -30; exit 1; }
  mkdir -p xz-prefix/lib xz-prefix/include
  cp "$(find xz-build -name 'liblzma.a' | sed -n 1p)" xz-prefix/lib/
  cp -r $XZ/src/liblzma/api/lzma $XZ/src/liblzma/api/lzma.h xz-prefix/include/
fi

# Mistral's data, made on the host by its own generator (native, with the host's liblzma): <die>-r.bin and global.bin,
# xz streams (its compression level 1, as Mistral's default); then a copy of Mistral that reads them from files
rm -rf mistral-host mistral-data && mkdir mistral-data
cmake -B mistral-host -S "$mistral_src" -DCMAKE_BUILD_TYPE=Release > mistral-host.log 2>&1 || { tail -30 mistral-host.log; exit 1; }
cmake --build mistral-host -j "$(nproc)" --target generator >> mistral-host.log 2>&1 || { tail -30 mistral-host.log; exit 1; }
for d in $dies -; do
  start=$(date +%s)
  mistral-host/generator/generator "$mistral_src/data" "$d" mistral-data 1 > /dev/null
  echo "Mistral's data $d made in $(( $(date +%s) - start )) s"
done
ls -la mistral-data
rm -rf mistral-wasi && cp -r "$mistral_src" mistral-wasi && rm -rf mistral-wasi/.git
node "$arch/ci/mistral-external-data.mjs" mistral-wasi

cmake -B nextpnr-build -S "$root" \
  -DCMAKE_TOOLCHAIN_FILE=$work/Toolchain-WASI.cmake -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=$work/$BOOST/stage/ -DBOOST_ROOT=$work/$BOOST -DSTATIC_BUILD=ON \
  -DEigen3_DIR=$work/eigen-prefix/share/eigen3/cmake \
  -DLIBLZMA_INCLUDE_DIR=$work/xz-prefix/include -DLIBLZMA_LIBRARY=$work/xz-prefix/lib/liblzma.a \
  -DPython3_EXECUTABLE="$(command -v python3)" -DPYTHON_EXECUTABLE="$(command -v python3)" -DBUILD_GUI=OFF -DBUILD_PYTHON=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_Threads=ON \
  -DARCH=mistral -DMISTRAL_ROOT=$work/mistral-wasi > nextpnr-configure.log 2>&1 || { tail -40 nextpnr-configure.log; exit 1; }
cmake --build nextpnr-build -j "$(nproc)" --target nextpnr-mistral > nextpnr-build.log 2>&1 || { grep -E "error" -A3 nextpnr-build.log | head -60; exit 1; }
wasm=$(find nextpnr-build -maxdepth 2 -name 'nextpnr-mistral*' -type f | sort | sed -n 1p)
[ -n "$wasm" ] || { echo "no nextpnr-mistral built"; exit 1; }
ls -la "$wasm"

# the package: global.bin in the runtime's resources (/share/mistral, every run reads it), each die's data a file of its
# own (data/<die>-r.bin, already xz-compressed), fetched by the package's data(part) only for the device in use
rm -rf npm && cp -r "$arch/npm" npm && mkdir -p npm/share/mistral npm/gen npm/data
cp mistral-data/global.bin npm/share/mistral/
cp "$root/COPYING" npm/LICENSE && cp "$mistral_src/LICENSE" npm/LICENSE-mistral
for d in $dies; do cp "mistral-data/$d-r.bin" npm/data/; done
# the models of those dies (Mistral's data/models.txt: a model's variant, a variant's die), for the package's dieOf
node -e '
  const fs = require("fs"), [file, out, ...dies] = process.argv.slice(1), die = {}, models = {};
  for (const l of fs.readFileSync(file, "utf8").split("\n")) {
    const v = /^variant (\S+) die:(\S+)/.exec(l); if (v) die[v[1]] = v[2];
    const m = /^model (\S+) v:(\S+)/.exec(l); if (m && dies.includes(die[m[2]])) models[m[1]] = die[m[2]];
  }
  fs.writeFileSync(out, JSON.stringify(models));
  console.log(`${Object.keys(models).length} models on ${dies.join(", ")}`);
' "$mistral_src/data/models.txt" npm/gen/models.json $dies
ls -la npm/share/mistral npm/data
# the version: package.json's; a release's tag must be it (RELEASE_VERSION, mistral-v0.1.0 -> v0.1.0), else a
# development one of it (0.1.0-dev.<commits>)
pkgversion=$(node -p 'require("./npm/package.json").version')
rv="${RELEASE_VERSION:-}"; version="${rv#v}"
if [ -n "$version" ] && [ "$version" != "$pkgversion" ]; then
  echo "the tag mistral-$rv is not package.json's version $pkgversion (npm/package.json, npm/CHANGELOG.md)" >&2; exit 1
fi
if [ -n "$version" ] && ! grep -q "^## $version\$" npm/CHANGELOG.md; then
  echo "npm/CHANGELOG.md has no section ## $version" >&2; exit 1
fi
version="${version:-$pkgversion-dev.$(git -C "$root" rev-list --count HEAD)}"
(cd npm && npm pkg set version="$version" > /dev/null && npm install --no-audit --no-fund > /dev/null \
  && npx jco new "$work/$wasm" --wasi-command --output nextpnr-mistral.wasm \
  && npx jco transpile nextpnr-mistral.wasm --instantiation async --no-typescript --no-namespaced-exports \
       --map 'wasi:io/*=runtime#io' --map 'wasi:cli/*=runtime#cli' --map 'wasi:clocks/*=runtime#*' \
       --map 'wasi:filesystem/*=runtime#fs' --map 'wasi:random/*=runtime#random' --out-dir gen/ \
  && npx yowasp-pack-resources gen/nextpnr-mistral-resources.js gen share \
  && npx esbuild --bundle lib/api.js --outfile=gen/bundle.js --format=esm --platform=node \
       --external:./resources-*.js "--define:VERSION=\"$version\"" \
  && mkdir -p "$work/dist" && npm pack --pack-destination "$work/dist")
ls -la "$work/dist"
