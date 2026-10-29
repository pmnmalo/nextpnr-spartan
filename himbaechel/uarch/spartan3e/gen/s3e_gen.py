#!/usr/bin/env python3
# Spartan-3E chip database for nextpnr-himbaechel, from Silinx's packed device graph
# (packages/silinx-fpga/devices/<part>.json.gz in Silinx: tiles, wires, PIPs, site pins and nodes,
# computed from ISE's `xdl -report -pips -all_conns`; see Silinx's fpga/device.js for the format).
#
# The graph has no site internals (XDL's primitive_defs are not kept), so the BELs inside a site and
# the site's internal wires are written here by hand, for the sites of the spike: SLICEL / SLICEM
# (two LUT4s, two flip-flops), IOB / IBUF (one pad buffer) and BUFGMUX.
#
#   s3e_gen.py <device.json.gz> <out.bba> [--sites sites.json] [--unknown-pips unknown.json]
#
# --sites: writes the XDL site name of each BEL's site ("X<c>Y<r>/<site index>"), for the converter
#   back to XDL (Silinx's research/nextpnr-s3e/nextpnr-to-xdl.mjs).
# --unknown-pips: the PIPs to leave out, by tile (Silinx's research/nextpnr-s3e/export-unknown-pips.mjs:
#   those whose bits Silinx's bit database does not know). The tiles of a template that leave out
#   different PIPs get tile types of their own.

import argparse, base64, gzip, json, re, struct, sys
from os import path
sys.path.append(path.join(path.dirname(__file__), "../../.."))
from himbaechel_dbgen.chip import *

PIP_ROUTETHRU = 2

# BELs per site: a site's BELs take z = site index * Z_STRIDE + the BEL's offset below
Z_STRIDE = 8
Z_FLUT, Z_GLUT, Z_FFX, Z_FFY = 0, 1, 2, 3
Z_IOB = 0
Z_BUFGMUX = 0


def u16s(b64):
    b = base64.b64decode(b64)
    return struct.unpack(f"<{len(b) // 2}H", b)


def varints(b64):
    b = base64.b64decode(b64)
    out, x, shift = [], 0, 0
    for byte in b:
        x |= (byte & 127) << shift
        if byte & 128:
            shift += 7
        else:
            out.append(x); x = 0; shift = 0
    return out


class Device:
    """Silinx's packed graph, decoded."""
    def __init__(self, file):
        with gzip.open(file, "rt") as f:
            p = json.load(f)
        if p.get("format") != "silinx-device":
            raise ValueError(f"{file}: not a Silinx device graph")
        self.part, self.family, self.rows, self.cols = p["part"], p["family"], p["rows"], p["cols"]
        names = p["names"]
        self.templates = []
        for tp in p["templates"]:
            wires = [names[i] for i in u16s(tp["wires"])]
            raw = u16s(tp["pips"])
            pips = [(raw[i], raw[i + 1], raw[i + 2]) for i in range(0, len(raw), 4)]
            sites = [(s["type"], [(pn, d, wires[w]) for pn, d, w in s["pins"]]) for s in tp["sites"]]
            self.templates.append((tp["type"], wires, pips, sites))
        self.tiles = [tuple(t) for t in p["tiles"]]   # name, row, col, template, [[site name, bonded]]
        # nodes: global wire ids (tile base + local wire), from the lowest
        base = [0]
        for t in self.tiles:
            base.append(base[-1] + len(self.templates[t[3]][1]))
        self.base = base
        v, i, prev, self.nodes = varints(p["nodes"]), 0, 0, []
        while i < len(v):
            first = prev + v[i]; prev = first
            n = v[i + 1] + 2; i += 2
            ids = [first]
            for _ in range(n - 1):
                ids.append(ids[-1] + v[i]); i += 1
            self.nodes.append(ids)

    def wire_at(self, wid):
        """(tile index, wire name) of a global wire id."""
        lo, hi = 0, len(self.tiles)
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if self.base[mid] <= wid: lo = mid
            else: hi = mid
        return lo, self.templates[self.tiles[lo][3]][1][wid - self.base[lo]]


def add_slice(tt, s, pins):
    """Two LUT4s (F, G) and two flip-flops (FFX, FFY) of a SLICEL / SLICEM; the shared CLK, CE and SR are
    the site's pins, so the two flip-flops' control sets are checked by the micro-architecture."""
    w = {pn: wn for pn, d, wn in pins}
    z = s * Z_STRIDE
    for lut, out, d, byp, ff, q, zl, zf in (("F", "X", "DX", "BX", "FFX", "XQ", Z_FLUT, Z_FFX),
                                            ("G", "Y", "DY", "BY", "FFY", "YQ", Z_GLUT, Z_FFY)):
        o = f"S{s}_{lut}OUT"; dw = f"S{s}_{d}"
        tt.create_wire(o, "LUT_OUT")
        tt.create_wire(dw, "FF_D")
        b = tt.create_bel(f"S{s}_{lut}LUT", "LUT4", z=z + zl)
        for i in range(4):
            tt.add_bel_pin(b, f"I{i}", w[f"{lut}{i + 1}"], PinType.INPUT)
        tt.add_bel_pin(b, "O", o, PinType.OUTPUT)
        tt.create_pip(o, w[out], timing_class="SITE")       # FXMUX / GYMUX: the LUT to X / Y
        tt.create_pip(o, dw, timing_class="SITE")           # DXMUX / DYMUX: the LUT ...
        tt.create_pip(w[byp], dw, timing_class="SITE")      # ... or BX / BY to the flip-flop
        f = tt.create_bel(f"S{s}_{ff}", "FF", z=z + zf)
        tt.add_bel_pin(f, "D", dw, PinType.INPUT)
        tt.add_bel_pin(f, "CLK", w["CLK"], PinType.INPUT)
        tt.add_bel_pin(f, "CE", w["CE"], PinType.INPUT)
        tt.add_bel_pin(f, "SR", w["SR"], PinType.INPUT)
        tt.add_bel_pin(f, "Q", w[q], PinType.OUTPUT)


def add_iob(tt, s, pins):
    """The pad buffer of an IOB / IBUF site: O (from the fabric, through O1), T (through T1), I (to the fabric)."""
    w = {pn: wn for pn, d, wn in pins}
    pad = f"S{s}_PAD"
    tt.create_wire(pad, "IO_PAD")
    b = tt.create_bel(f"S{s}_IOB", "IOB", z=s * Z_STRIDE + Z_IOB)
    if "O1" in w:                                       # (not on the input-only sites)
        tt.add_bel_pin(b, "O", w["O1"], PinType.INPUT)
        tt.add_bel_pin(b, "T", w["T1"], PinType.INPUT)
    tt.add_bel_pin(b, "I", w["I"], PinType.OUTPUT)
    tt.add_bel_pin(b, "PAD", pad, PinType.INOUT)


def add_bufgmux(tt, s, pins):
    w = {pn: wn for pn, d, wn in pins}
    b = tt.create_bel(f"S{s}_BUFGMUX", "BUFGMUX", z=s * Z_STRIDE + Z_BUFGMUX)
    tt.add_bel_pin(b, "I0", w["I0"], PinType.INPUT)
    tt.add_bel_pin(b, "O", w["O"], PinType.OUTPUT)


SITE_BELS = {"SLICEL": add_slice, "SLICEM": add_slice, "IOB": add_iob, "DIFFM": add_iob, "DIFFS": add_iob,
             "IBUF": add_iob, "DIFFMI": add_iob, "DIFFSI": add_iob,
             "BUFGMUX": add_bufgmux}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("device")
    ap.add_argument("bba")
    ap.add_argument("--sites")
    ap.add_argument("--unknown-pips")
    args = ap.parse_args()
    dev = Device(args.device)
    unknown = {}
    if args.unknown_pips:
        with open(args.unknown_pips) as f:
            unknown = json.load(f)
    ch = Chip("spartan3e", dev.part, dev.cols, dev.rows)
    ch.strs.read_constids(path.join(path.dirname(__file__), "../constids.inc"))
    # tile types: one per template of the graph and set of PIPs left out
    tile_types = {}
    def tile_type(t, left_out):
        key = (t, tuple(left_out))
        if key not in tile_types:
            ttype, wires, pips, sites = dev.templates[t]
            name = f"{ttype}_{t}" + (f"_{len(tile_types)}" if left_out else "")
            tt = ch.create_tile_type(name)
            for wn in wires:
                tt.create_wire(wn)
            skip = set(left_out)
            for i, (a, b, flags) in enumerate(pips):
                if flags & PIP_ROUTETHRU or i in skip:
                    continue
                tt.create_pip(wires[a], wires[b])
            for s, (stype, pins) in enumerate(sites):
                if stype in SITE_BELS:
                    SITE_BELS[stype](tt, s, pins)
            tile_types[key] = name
        return tile_types[key]
    site_names = {}
    pkg = ch.create_package(re.match(r"xc3s\d+e(\w+?)-", dev.part).group(1).upper())
    for name, r, c, t, sites in dev.tiles:
        ch.set_tile_type(c, r, tile_type(t, unknown.get(name, [])))
        for s, (sname, bonded) in enumerate(sites):
            stype = dev.templates[t][3][s][0]
            if stype not in SITE_BELS:
                continue
            site_names[f"X{c}Y{r}/{s}"] = sname
            if SITE_BELS[stype] is add_iob and bonded == 1:
                pkg.create_pad(sname, f"X{c}Y{r}", f"S{s}_IOB", "", 0)
    for ids in dev.nodes:
        ch.add_node([NodeWire(dev.tiles[k][2], dev.tiles[k][1], wn) for k, wn in map(dev.wire_at, ids)])
    ch.set_speed_grades(["4"])
    ch.write_bba(args.bba)
    if args.sites:
        with open(args.sites, "w") as f:
            json.dump(site_names, f)
    print(f"{dev.part}: {len(tile_types)} tile types, {len(dev.nodes)} nodes", file=sys.stderr)


if __name__ == "__main__":
    main()
