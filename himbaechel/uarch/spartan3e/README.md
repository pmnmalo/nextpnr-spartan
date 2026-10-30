# spartan3e: the Xilinx Spartan-3E in nextpnr-himbaechel

A Himbaechel micro-architecture for the Spartan-3E, developed for [Silinx ISE](https://github.com/pmnmalo/Silinx-ISE)
(the plan: [Silinx #2131](https://github.com/pmnmalo/Silinx-ISE/issues/2131)).

## The chip database

`gen/s3e_gen.py` builds it from Silinx's device graph of a part, `packages/silinx-fpga/devices/<part>.json.gz`:
- **Routing:** the tiles, wires, PIPs and nodes, computed by Silinx from ISE 14.7's `xdl -report -pips -all_conns`.
- **Sites:** the BELs inside them, written by hand: per slice two LUT4s, two flip-flops, the carry logic (two MUXCYs,
  two XORCYs), the F5 multiplexer and the FXMUX: an F6 on the lower slices of a CLB, the F7 on X0Y1, the F8 on X1Y1; per IOB the pad buffer and its registers (IFF1 / IFF2, OFF1 / OFF2, TFF1, and the
  multiplexers between them and the pad), the BUFGMUX, the block RAM (RAMB16) and the multiplier (MULT18X18SIO), one
  BEL each with the site's pins (not the multiplier's cascade yet); a constant 1 on each VCC site (VCCSRC) and a
  constant 0 on Y of each SLICEL used for nothing else (GNDSRC), for the block RAMs' and multipliers' constant pins:
  XDL's `vcc` and `gnd` nets, which ISE takes for constants.

The packer puts a flip-flop into its pad's IOB when asked (the attribute `IOB` on it or on the pad's buffer, which
Silinx writes from the UCF's `IOB = TRUE`), as ISE's map does; an IDDR2 / ODDR2 is always there, as two flip-flops (the
second on the falling edge when its clock is the first's inverted). A global buffer driven by a clock pad is placed
where the pad's dedicated path reaches it, and the router takes that path.

Two optional inputs come from Silinx's scripts (`packages/silinx-fpga/research/nextpnr-s3e/`):
- `--unknown-pips`: the PIPs whose bits Silinx's bit database does not know, left out (`export-unknown-pips.mjs`);
- `--timing`: Silinx's delay model, fitted to ISE's `trce` (`export-timing.mjs`). Each PIP takes the delay of the
  node it drives; the LUT and flip-flop arcs come from the slice's delays.

## Build

```
node <silinx>/packages/silinx-fpga/research/nextpnr-s3e/export-unknown-pips.mjs xc3s250ecp132-4 <dir>/unknown-pips-xc3s250e.json
node <silinx>/packages/silinx-fpga/research/nextpnr-s3e/export-timing.mjs xc3s250ecp132-4 <dir>/timing-xc3s250e.json
cmake -S . -B build -DARCH=himbaechel -DHIMBAECHEL_UARCH=spartan3e -DBUILD_PYTHON=OFF -DBUILD_GUI=OFF \
  -DSILINX_DEVICES=<silinx>/packages/silinx-fpga/devices -DSILINX_UNKNOWN_PIPS=<dir> -DSILINX_TIMING=<dir> \
  -DHIMBAECHEL_SPARTAN3E_DEVICES="xc3s250e:cp132"
cmake --build build -j
```

## Run

The netlist comes from Yosys, `synth_xilinx -family xc3se` (Silinx's synthesis); the ports' pads are `LOC`
attributes:

```
build/nextpnr-himbaechel --device xc3s250ecp132-4 --json design.json --write routed.json
```

The routed design goes to XDL, and from there to a `.bit`, with Silinx's scripts:
- `nextpnr-to-xdl.mjs` converts it;
- `xdl-sim-check.mjs` checks the XDL against the netlist;
- `xdl-to-bit.mjs` writes the bitstream.

## Tests

`tests/` holds Yosys netlists of test designs. The fork's CI (`.github/workflows/spartan3e.yml`) places and routes
each one, and checks there's no error and the maximum frequency nextpnr reports.

## WebAssembly

`ci/build-wasm.sh` builds `nextpnr-himbaechel-spartan3e` for single-threaded WASI and packages it for npm, as YoWASP
packages nextpnr (`npm/`: `@silinx/nextpnr-himbaechel-spartan3e`, run by `@yowasp/runtime`). Linux only.
- **Toolchain:** the WASI SDK 34, with C++ exceptions as WebAssembly exceptions; nextpnr needs them since it dropped
  its WASI workarounds. No LTO: exceptions and LTO don't yet work together (WebAssembly/wasi-sdk#629). Boost 1.81 and
  Eigen 3.4 are built for it.
- **The package:** the chip databases are kept outside the binary and read from `/share` (the package's resources).
  `jco` 1.19.0 and `@yowasp/runtime` 11.0.72, as `@yowasp/yosys` 0.70.

The CI's `wasm` job builds it, then runs the package in Node on the test netlists (`ci/test-wasm.mjs`): the maximum
frequency must equal the native build's exactly.

The chip databases are not the runtime's resources, since the runtime fetches all of its resources on the first run.
Each device's is a file of its own, `chipdb/chipdb-<device>.bin.gz`:
- `chipdb(part)` reads it from disk in Node, or fetches it over HTTP in a browser, and decompresses it;
- the caller gives it to nextpnr as a file, with `--chipdb`;
- only the device in use is fetched.

Sizes (2026-10-30):
- `nextpnr-himbaechel-spartan3e.wasm`: 1.76 MB;
- the chip databases, gzipped (as downloaded) / unpacked:

  | device | gzipped | unpacked |
  |---|---|---|
  | XC3S100E | 4.5 MB | 19.9 MB |
  | XC3S250E | 5.3 MB | 24.3 MB |
  | XC3S500E | 5.5 MB | 27.0 MB |
  | XC3S1200E | 6.1 MB | 32.3 MB |

- the npm package: 19.6 MB.

Reading and decompressing a chip database takes 0.1 s; a run takes 0.2 s on the test netlists.
