# Security

This fork (nextpnr-spartan) adds the Xilinx Spartan-3E to nextpnr for [Silinx ISE](https://github.com/pmnmalo/Silinx-ISE).

## Reporting a vulnerability

Report a vulnerability privately, not in a public issue: on GitHub, Security ▸ Report a vulnerability, at
<https://github.com/pmnmalo/nextpnr-spartan/security/advisories/new>. Say what you found, how to reproduce it and
what it allows. You get an answer within two weeks.

## Scope

- **In scope:** what this fork adds or changes:
  - the `spartan3e` micro-architecture (`himbaechel/uarch/spartan3e/`) and its chip database generator;
  - the fork's workflows (`.github/workflows/spartan3e.yml`, `upstream-sync.yml`);
  - the WebAssembly build and package.
- **Upstream nextpnr's own code:** report to [YosysHQ/nextpnr](https://github.com/YosysHQ/nextpnr). The fork follows
  upstream weekly, so a fix there reaches it.
- **Silinx itself:** its own [security policy](https://github.com/pmnmalo/Silinx-ISE/blob/main/SECURITY.md).

## Supported versions

The `silinx` branch and the latest release of the WebAssembly package. Older ones are not patched.
