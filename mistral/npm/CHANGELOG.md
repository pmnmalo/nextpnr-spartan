# Changelog of @silinx/nextpnr-mistral

Each version is the fork's tag `mistral-v<version>`, its GitHub release (the package) and its npm package. A version's
changes are written here before it is tagged. Before 1.0.0 a minor version may change the interface the
[README](README.md) describes.

## 0.1.0

The first: nextpnr-mistral in WebAssembly (pmnmalo/Silinx#5377), with Mistral's data of the e50f (the Cyclone V E A2 / A4:
the DE0-CV's 5CEBA4F23C7) and the sx120f (the Cyclone V SE A5 / A6: the DE1-SoC's 5CSEMA5F31C6, the DE10-Nano's
5CSEBA6U23I7). Mistral's routing data read from files rather than linked in (mistral/ci/mistral-external-data.mjs).
