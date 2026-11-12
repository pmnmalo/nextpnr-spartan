# Changelog of @silinx/nextpnr-himbaechel-xc7

Each version is the fork's tag `xc7-v<version>`, its GitHub release (the package and the chip databases) and its npm
package. A version's changes are written here before it is tagged. Before 1.0.0 a minor version may change the interface
the [README](README.md) describes.

## Unreleased

- Two LUTs of at most five inputs in all, sharing at least one, packed in one site's 6LUT and 5LUT, as Vivado combines
  them (pmnmalo/Silinx#3495): on Silinx's examples 7-16 % fewer LUTs as Vivado counts them (vga 119 to 106, Vivado's
  105). The tests' LUTs as Vivado counts them now at most 5 % over Vivado's (15 % before).

## 0.1.1

The same nextpnr and chip databases as 0.1.0, published again: npm kept 0.1.0 staged when its first, manual publication was not completed, and refuses that version number.

## 0.1.0

The first version (pmnmalo/Silinx#3143, after the spike of pmnmalo/Silinx#2305): nextpnr-himbaechel's `xilinx`
micro-architecture (the 7-series, on Project X-Ray's database) in WebAssembly, as the Spartan-3E's package is built.
- The chip databases of the xc7a50t (which the xc7a35t takes) and the xc7a100t, gzipped, from prjxray-db
  0a0addedd73e7e4139d52a6d8db4258763e0f1f3 (`PRJXRAY_DB_REF` of `.github/workflows/xilinx.yml`).
- The carry chains packed as Vivado packs them, feed-through LUTs only where the site needs one (pmnmalo/Silinx#3160).
- Tested in Node and in headless Chromium on Silinx's blinky, vga and micro80 on the xc7a35tcpg236-1 with router 1, with
  the native build's checks (`tests/expected.txt`): the feed-through LUTs and LUT cells, the LUTs as Vivado counts them
  at most 15 % over Vivado's, no warning, the maximum frequency equal to the native build's.
