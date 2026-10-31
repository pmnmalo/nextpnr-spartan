#!/usr/bin/env python3
# Spartan-6 chip database for nextpnr-himbaechel, from Silinx's packed device graph
# (packages/silinx-fpga/devices/<part>.json.gz in Silinx: tiles, wires, PIPs, site pins and nodes, computed from ISE's
# `xdl -report -pips -all_conns`).
#
# One BEL per site, its pins the site's: Silinx packs the design into sites (its fpga/s6/pack.js and phys.js, whose
# settings ISE's DRC and bitgen accept) and this micro-architecture places and routes those sites.
#
#   s6_gen.py <device.json.gz> <out.bba> [--unknown-pips unknown.json] [--timing timing.json]
#
# --unknown-pips: the PIPs to leave out, by tile (Silinx's research/nextpnr-s6/export-unknown-pips.mjs: those whose
#   bits Silinx's bit database does not know).
# --timing: Silinx's delay model (research/nextpnr-s6/export-timing.mjs: fitted to ISE's trce): each PIP takes the
#   delay of the class of the wire it drives.
#
# A PIP's flags (spartan6.cc reads them):
#   FLAG_GENERAL: it drives the general interconnect, which the global clocks do not take;
#   FLAG_RT_FREE: a route-through of the I/O logic or a BUFH, for a net when the design has no cell on the site;
#   FLAG_RT_OWN: a slice's COUT -> DMUX, for the net of that slice's carry out when DMUX is free;
#   the site's index above FLAG_SITE_SHIFT.

import argparse, json, re, sys
from os import path
sys.path.append(path.join(path.dirname(__file__), "../../.."))
sys.path.append(path.join(path.dirname(__file__), "../../spartan3e/gen"))
from himbaechel_dbgen.chip import *
from s3e_gen import Device, u16s, ps

PIP_ROUTETHRU = 2
FLAG_GENERAL, FLAG_RT_FREE, FLAG_RT_OWN = 1, 2, 4
FLAG_SITE_SHIFT = 8

# The sites that get a BEL, by the BEL's type
BEL_TYPE = {"SLICEX": "SLICEX", "SLICEL": "SLICEL", "SLICEM": "SLICEM", "IOBM": "IOB", "IOBS": "IOB",
            "BUFGMUX": "BUFGMUX", "RAMB16BWER": "RAMB16BWER", "RAMB8BWER": "RAMB8BWER", "DSP48A1": "DSP48A1",
            "TIEOFF": "TIEOFF", "DCM": "DCM", "PLL_ADV": "PLL_ADV"}
FREE_ROUTETHRU = {"ILOGIC2", "OLOGIC2", "BUFH"}
# a switch box's wires of the general interconnect (Silinx's fpga/s6/route-nets.js GENERAL_WIRE)
GENERAL_WIRE = re.compile(r"^((EE|EL|ER|NE|NL|NN|NR|NW|SE|SL|SR|SS|SW|WL|WR|WW)[124][A-Z]|LOGICIN|LOGICOUT|FAN|GFAN|KEEP\d|VCC_WIRE|GND_WIRE)")
ROUTETHRU = re.compile(r"^_ROUTETHROUGH-(\w+)-(\w+) (\w+)$")


def general_wires(dev):
    """Per template, the wires on a node of the general interconnect in some tile (a node's name in another tile may
    not say so)."""
    general = [set() for _ in dev.templates]
    for ids in dev.nodes:
        at = [dev.wire_at(w) for w in ids]
        if any(GENERAL_WIRE.match(wn) for _, wn in at):
            for k, wn in at:
                general[dev.tiles[k][3]].add(wn)
    return general


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("device")
    ap.add_argument("bba")
    ap.add_argument("--unknown-pips")
    ap.add_argument("--timing")
    args = ap.parse_args()
    timing = None
    if args.timing:
        with open(args.timing) as f:
            timing = json.load(f)
    unknown = {}
    if args.unknown_pips:
        with open(args.unknown_pips) as f:
            unknown = json.load(f)
    dev = Device(args.device)
    with __import__("gzip").open(args.device, "rt") as f:
        raw = json.load(f)
    routethrus = raw["routethrus"]
    rt_of = [u16s(tp["pips"])[3::4] for tp in raw["templates"]]
    general = general_wires(dev)
    ch = Chip("spartan6", dev.part, dev.cols, dev.rows)
    ch.strs.read_constids(path.join(path.dirname(__file__), "../constids.inc"))

    def pip_flags(t, i, a, b, sites_at):
        """The flags of a PIP of template t, or None to leave it out (a route-through nothing may take)."""
        flags = FLAG_GENERAL if b in general[t] else 0
        if not dev.templates[t][2][i][2] & PIP_ROUTETHRU:
            return flags
        m = ROUTETHRU.match(routethrus[rt_of[t][i] - 1])   # (the graph keeps the index + 1)
        s = sites_at.get(a, sites_at.get(b))
        if not m or s is None:
            return None
        if m.group(3) in FREE_ROUTETHRU:
            return flags | FLAG_RT_FREE | (s << FLAG_SITE_SHIFT)
        if m.group(3).startswith("SLICE") and m.group(1) == "COUT" and m.group(2) == "DMUX":
            return flags | FLAG_RT_OWN | (s << FLAG_SITE_SHIFT)
        return None

    tile_types = {}
    def tile_type(t, left_out):
        key = (t, tuple(left_out))
        if key not in tile_types:
            ttype, wires, pips, sites = dev.templates[t]
            name = f"{ttype}_{t}" + (f"_{len(tile_types)}" if left_out else "")
            tt = ch.create_tile_type(name)
            for wn in wires:
                tt.create_wire(wn)
            sites_at = {}
            for s, (stype, pins) in enumerate(sites):
                for _, _, wn in pins:
                    sites_at.setdefault(wn, s)
            skip = set(left_out)
            classes = timing["pipClass"][str(t)] if timing else None
            for i, (a, b, _) in enumerate(pips):
                flags = None if i in skip else pip_flags(t, i, wires[a], wires[b], sites_at)
                if flags is None:
                    continue
                p = tt.create_pip(wires[a], wires[b], timing_class=f"W_{classes[i]}" if classes else "")
                p.flags = flags
            for s, (stype, pins) in enumerate(sites):
                if stype in BEL_TYPE:
                    b = tt.create_bel(f"S{s}_{BEL_TYPE[stype]}", BEL_TYPE[stype], z=s)
                    for pn, d, wn in pins:
                        tt.add_bel_pin(b, pn, wn, PinType.OUTPUT if d == "o" else PinType.INPUT)
            tile_types[key] = name
        return tile_types[key]

    pkg = ch.create_package(re.match(r"xc6slx\d+(\w+?)-", dev.part).group(1).upper())
    for name, r, c, t, sites in dev.tiles:
        ch.set_tile_type(c, r, tile_type(t, unknown.get(name, [])))
        for s, (sname, bonded) in enumerate(sites):
            if dev.templates[t][3][s][0] in ("IOBM", "IOBS") and bonded == 1:
                pkg.create_pad(sname, f"X{c}Y{r}", f"S{s}_IOB", "", 0)
    for ids in dev.nodes:
        ch.add_node([NodeWire(dev.tiles[k][2], dev.tiles[k][1], wn) for k, wn in map(dev.wire_at, ids)])
    ch.set_speed_grades(["3"])
    if timing:
        for c, d in enumerate(timing["wire"]):
            ch.timing.set_pip_class("3", f"W_{c}", ps(d))
    ch.write_bba(args.bba)
    print(f"{dev.part}: {len(tile_types)} tile types, {len(dev.nodes)} nodes", file=sys.stderr)


if __name__ == "__main__":
    main()
