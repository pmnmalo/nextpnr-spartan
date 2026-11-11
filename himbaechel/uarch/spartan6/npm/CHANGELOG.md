# Changelog of @silinx/nextpnr-himbaechel-spartan6

Each version is the fork's tag `spartan6-v<version>`, its GitHub release (the package and the chip databases) and its
npm package. A version's changes are written here before it is tagged. Before 1.0.0 a minor version may change the
interface the [README](README.md) describes.

## 0.4.0

The chip databases built from a newer Silinx commit (`SILINX_REF`). That commit has the PIPs Silinx has measured with ISE
but keeps out of its own flow until this chip database has them: its `bitdb/s6/s6-pips-held.json` (pmnmalo/Silinx#3349),
which `export-unknown-pips.mjs` counts as known.
- The slave OLOGIC2's `D1 -> OQ` route-through in the top bank's TIOI_INNER tiles (pmnmalo/Silinx#3269). With it, a port
  LOC'd on one of their pads (A2 among them: 14 pads of the XC6SLX16, 11 of the XC6SLX45) takes an output. 0.3.0 failed
  on such a port ("Failed to route arc ... to X5Y0/TIOB_O3_PINW", pmnmalo/Silinx#3152). Its delay is the nodes' delay
  from Silinx's timing model, as for every PIP.
- The BSCAN corner tiles' PIPs that Silinx measured since 0.3.0 (pmnmalo/Silinx#3254). The packer does not place a
  BSCAN_SPARTAN6 yet.
- The tests also route `tests/pads.txt`'s netlists on both devices, with no warning; the native build makes both
  devices' chip databases.

## 0.3.1

The package on npm (pmnmalo/Silinx#3141), as `@silinx/nextpnr-himbaechel-spartan3e` is: a `spartan6-v*` tag's run stages
it with its provenance (npm's trusted publishing), public once the maintainer approves it.
- nextpnr and the chip databases are 0.3.0's (the same code, the same `SILINX_REF`): the same placements, routes and
  frequencies.
- The version is `npm/package.json`'s, with its section here: `ci/build-wasm.sh` refuses a tag that is not it.
- This changelog and a README in the package.

## 0.3.0 and before

0.1.0 to 0.3.0 were GitHub releases of the fork only (the tags `spartan6-v0.1.0` … `spartan6-v0.3.0`, the package's
`.tgz` and the chip databases attached), which Silinx installed from the release's `.tgz`.
