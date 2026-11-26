// Mistral (github.com/Ravenslofty/mistral, at the workflow's MISTRAL_REF) made to read its routing and global data from
// files at run time, for the WebAssembly build (pmnmalo/Silinx#5377): Mistral links each die's routing data (<die>-r.bin)
// and the global data (global.bin) into libmistral with `ld -r -b binary`, which makes host objects, not WebAssembly
// ones, and its generator, which makes them, would be built for WebAssembly too. Here, in a checkout of Mistral:
//   - libmistral's CMakeLists.txt: no generated .bin nor `ld -r`, liblzma's headers found; their symbols (_binary_<name>_bin_start / _end) are
//     markers, "MISTRAL-FILE:<name>.bin", defined in cv-external-data.cc;
//   - CycloneV::get_bin: a marker's file read from the working directory, else from /share/mistral (the package's
//     resources), then decompressed as before (the .bin files are xz streams);
//   - generator/CMakeLists.txt: the generator not built when cross-compiling (it runs on the host: build-wasm.sh);
//   - libmistral's sources: <algorithm> included (std::fill: the WASI SDK's libc++ does not bring it in through others).
//   node mistral-external-data.mjs <mistral checkout>
import fs from 'node:fs';
import path from 'node:path';

const root = path.resolve(process.argv[2] ?? '');
const DIES = ['e50f', 'gx25f', 'gt75f', 'gt150f', 'gt300f', 'sx50f', 'sx120f'];

function edit(file, from, to) {
  const p = path.join(root, file);
  const text = fs.readFileSync(p, 'utf8');
  if (!text.includes(from)) throw new Error(`${file}: not found (Mistral changed?): ${from.split('\n')[0]}`);
  fs.writeFileSync(p, text.replace(from, to));
}

// libmistral: the markers instead of the linked-in data
const cm = 'libmistral/CMakeLists.txt';
const cmText = fs.readFileSync(path.join(root, cm), 'utf8');
const start = cmText.indexOf('# Binary routing/timing information generation');
const endMark = 'list(APPEND mistral_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/cv-global.o)\n';
const end = cmText.indexOf(endMark);
if (start < 0 || end < 0) throw new Error(`${cm}: the binary data's section not found (Mistral changed?)`);
fs.writeFileSync(path.join(root, cm), cmText.slice(0, start)
  + '# (the routing and global data read from files at run time: cv-external-data.cc, CycloneV::get_bin)\n'
  + 'list(APPEND mistral_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/cv-external-data.cc)\n'
  + cmText.slice(end + endMark.length));
edit(cm, 'add_dependencies(mistral generator)\n', '');
// (liblzma's headers: the build's, not the host's, which a cross build does not see)
edit(cm, 'add_library(mistral ${mistral_SOURCES})\n', 'add_library(mistral ${mistral_SOURCES})\nfind_package(LibLZMA REQUIRED)\ntarget_include_directories(mistral PRIVATE ${LIBLZMA_INCLUDE_DIRS})\n');

// (the symbol e50f_r's file e50f-r.bin, as the generator names it)
const marker = name => `extern const uint8_t _binary_${name}_bin_start[] = "MISTRAL-FILE:${name.replace('_', '-')}.bin";\n`
  + `extern const uint8_t _binary_${name}_bin_end[] = "";\n`;
fs.writeFileSync(path.join(root, 'libmistral/cv-external-data.cc'),
  '// The routing data of each die and the global data, read from files at run time (CycloneV::get_bin): markers in\n'
  + '// place of the data Mistral links in (nextpnr-spartan\'s mistral/ci/mistral-external-data.mjs).\n'
  + '#include <stdint.h>\n\n' + [...DIES.map(d => `${d}_r`), 'global'].map(marker).join(''));

// get_bin: a marker's file read, then decompressed as before
edit('libmistral/cyclonev.cc', '#include <lzma.h>\n', '#include <lzma.h>\n#include <stdio.h>\n#include <string>\n');
edit('libmistral/cyclonev.cc', `  const uint8_t *data = start;
  size_t dsize = end - start;
`, `  const uint8_t *data = start;
  size_t dsize = 0;
  static const char marker[] = "MISTRAL-FILE:";
  if(!memcmp(start, marker, sizeof(marker) - 1)) {
    std::string name = reinterpret_cast<const char *>(start) + sizeof(marker) - 1;
    FILE *fd = fopen(name.c_str(), "rb");
    if(!fd)
      fd = fopen(("/share/mistral/" + name).c_str(), "rb");
    if(!fd) {
      fprintf(stderr, "ERROR: Mistral's data file %s not found (in the working directory or /share/mistral)\\n", name.c_str());
      exit(1);
    }
    fseek(fd, 0, SEEK_END);
    dsize = ftell(fd);
    fseek(fd, 0, SEEK_SET);
    decompressed_data_storage.emplace_back(std::make_unique<uint8_t[]>(dsize));
    if(fread(decompressed_data_storage.back().get(), 1, dsize, fd) != dsize) {
      fprintf(stderr, "ERROR: Mistral's data file %s not read\\n", name.c_str());
      exit(1);
    }
    fclose(fd);
    data = decompressed_data_storage.back().get();
  } else
    dsize = end - start;
`);

// the generator: the host's (build-wasm.sh builds it natively and makes the .bin files)
edit('generator/CMakeLists.txt', 'project(generator)\n', 'project(generator)\nif(NOT CMAKE_CROSSCOMPILING)\n');
fs.appendFileSync(path.join(root, 'generator/CMakeLists.txt'), 'endif()\n');
for (const f of fs.readdirSync(path.join(root, 'libmistral')).filter(f => f.endsWith('.cc'))) {
  const p = path.join(root, 'libmistral', f), src = fs.readFileSync(p, 'utf8');
  if (!/#include <algorithm>/.test(src)) fs.writeFileSync(p, `#include <algorithm>\n${src}`);
}
console.log(`Mistral in ${root}: its data read from files (${DIES.length} dies' markers and global.bin's)`);
