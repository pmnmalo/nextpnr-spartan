#!/usr/bin/env python3
# Spartan-3E chip database for nextpnr-himbaechel, from Silinx's packed device graph
# (packages/silinx-fpga/devices/<part>.json.gz in Silinx: tiles, wires, PIPs, site pins and nodes,
# computed from ISE's `xdl -report -pips -all_conns`; see Silinx's fpga/device.js for the format).
#
# The graph has no site internals (XDL's primitive_defs are not kept), so the BELs inside a site and
# the site's internal wires are written here by hand: SLICEL / SLICEM (two LUT4s, two flip-flops, the carry logic,
# the F5 multiplexer and the FXMUX: F6, F7 or F8), IOB / IBUF (one pad
# buffer, its flip-flops), BUFGMUX, and the block RAM (RAMB16) and multiplier (MULT18X18SIO), one BEL each with the
# site's pins.
#
#   s3e_gen.py <device.json.gz> <out.bba> [--sites sites.json] [--unknown-pips unknown.json] [--timing timing.json]
#
# --sites: writes the XDL site name of each BEL's site ("X<c>Y<r>/<site index>"), for the converter
#   back to XDL (Silinx's research/nextpnr-s3e/nextpnr-to-xdl.mjs).
# --unknown-pips: the PIPs to leave out, by tile (Silinx's research/nextpnr-s3e/export-unknown-pips.mjs:
#   those whose bits Silinx's bit database does not know). The tiles of a template that leave out
#   different PIPs get tile types of their own.
# --timing: Silinx's delay model (research/nextpnr-s3e/export-timing.mjs: fitted to ISE's trce): each PIP
#   takes the delay of the class of the node it drives (nextpnr counts the PIPs' delays, not the wires'),
#   the LUTs and the flip-flops the slice's delays. Not the loads (Silinx's delay per extra branch a net
#   takes off a node): as nextpnr's RC model, which its router does not see, they made the routes slower.

import argparse, base64, gzip, json, re, struct, sys
from os import path
sys.path.append(path.join(path.dirname(__file__), "../../.."))
from himbaechel_dbgen.chip import *

PIP_ROUTETHRU = 2

# BELs per site: a site's BELs take z = site index * Z_STRIDE + the BEL's offset below
Z_STRIDE = 16
Z_FLUT, Z_GLUT, Z_FFX, Z_FFY = 0, 1, 2, 3
Z_CYMUXF, Z_CYMUXG, Z_XORF, Z_XORG = 4, 5, 6, 7
Z_F5MUX, Z_FXMUX = 8, 9
Z_FMEM, Z_GMEM = 10, 11
Z_GND = 15
Z_IOB, Z_IFF1, Z_IFF2, Z_OFF1, Z_OFF2, Z_TFF1 = 0, 1, 2, 3, 4, 5
Z_BUFGMUX = 0
Z_HARD = 0


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


def add_slice(tt, s, pins, pips):
    """Two LUT4s (F, G) and two flip-flops (FFX, FFY) of a SLICEL / SLICEM; the shared CLK, CE and SR are
    the site's pins, so the two flip-flops' control sets are checked by the micro-architecture. A LUT's
    inputs are permutable: its inputs I0..I3 are wires of their own, each reached from every one of the site's
    pins F1..F4 (G1..G4) by a PIP (LUTPIN1..4, the pin's own delay), so that the router chooses the pins; the
    converter back to XDL rewrites the LUT's equation for the pins chosen."""
    w = {pn: wn for pn, d, wn in pins}
    z = s * Z_STRIDE
    for lut, out, d, byp, ff, q, zl, zf in (("F", "X", "DX", "BX", "FFX", "XQ", Z_FLUT, Z_FFX),
                                            ("G", "Y", "DY", "BY", "FFY", "YQ", Z_GLUT, Z_FFY)):
        o = f"S{s}_{lut}OUT"; dw = f"S{s}_{d}"
        tt.create_wire(o, "LUT_OUT")
        tt.create_wire(dw, "FF_D")
        b = tt.create_bel(f"S{s}_{lut}LUT", "LUT4", z=z + zl)
        for i in range(4):
            li = f"S{s}_{lut}I{i}"
            tt.create_wire(li, "LUT_IN")
            tt.add_bel_pin(b, f"I{i}", li, PinType.INPUT)
            for k in range(4):
                tt.create_pip(w[f"{lut}{k + 1}"], li, timing_class=f"LUTPIN{k + 1}")
        tt.add_bel_pin(b, "O", o, PinType.OUTPUT)
        tt.create_pip(o, w[out], timing_class="SITE")       # FXMUX / GYMUX: the LUT to X / Y
        tt.create_pip(o, dw, timing_class="SITE")           # DXMUX / DYMUX: the LUT ...
        tt.create_pip(w[byp], dw, timing_class="BYPASS")    # ... or BX / BY to the flip-flop
        f = tt.create_bel(f"S{s}_{ff}", "FF", z=z + zf)
        tt.add_bel_pin(f, "D", dw, PinType.INPUT)
        tt.add_bel_pin(f, "CLK", w["CLK"], PinType.INPUT)
        tt.add_bel_pin(f, "CE", w["CE"], PinType.INPUT)
        tt.add_bel_pin(f, "SR", w["SR"], PinType.INPUT)
        tt.add_bel_pin(f, "Q", w[q], PinType.OUTPUT)
    add_carry(tt, s, w)
    add_muxes(tt, s, w, pips)


def add_carry(tt, s, w):
    """The slice's carry logic: CYINIT (the chain's carry in from CIN, or BX at its start), per side a MUXCY (CYMUXF /
    CYMUXG: S the LUT's output, DI from CY0F / CY0G: pin 1 of the LUT, BX / BY, or a constant set in the cell) and an
    XORCY (XORF / XORG: LI the LUT's output), the sum out through FXMUX / GYMUX to X / Y and to the flip-flop. CYMUXG's
    output is the COUT pin, which the graph's COUT -> CIN PIPs take to the next slice up the column."""
    z = s * Z_STRIDE
    ci, cyf = f"S{s}_CI", f"S{s}_CYF"
    tt.create_wire(ci, "CARRY")
    tt.create_wire(cyf, "CARRY")
    tt.create_pip(w["CIN"], ci, timing_class="SITE")       # CYINIT: the carry of the slice below ...
    tt.create_pip(w["BX"], ci, timing_class="SITE")        # ... or BX, at the chain's start
    tt.create_pip(cyf, w["XB"], timing_class="XB")         # XBUSED, YBUSED: the carry out of a stage read by logic
    tt.create_pip(w["COUT"], w["YB"], timing_class="SITE")
    for side, lut_out, carry_in, carry_out, pin1, byp, out, d, zm, zx in (
            ("F", f"S{s}_FOUT", ci, cyf, "F1", "BX", "X", f"S{s}_DX", Z_CYMUXF, Z_XORF),
            ("G", f"S{s}_GOUT", cyf, w["COUT"], "G1", "BY", "Y", f"S{s}_DY", Z_CYMUXG, Z_XORG)):
        di, xo = f"S{s}_DI{side}", f"S{s}_XOR{side}"
        tt.create_wire(di, "CARRY")
        tt.create_wire(xo, "CARRY")
        tt.create_pip(w[pin1], di, timing_class="SITE")    # CY0F / CY0G: pin 1 of the LUT ...
        tt.create_pip(w[byp], di, timing_class="SITE")     # ... or BX / BY (a constant: the cell's CY0)
        m = tt.create_bel(f"S{s}_CYMUX{side}", "MUXCY", z=z + zm)
        tt.add_bel_pin(m, "CI", carry_in, PinType.INPUT)
        tt.add_bel_pin(m, "DI", di, PinType.INPUT)
        tt.add_bel_pin(m, "S", lut_out, PinType.INPUT)
        tt.add_bel_pin(m, "O", carry_out, PinType.OUTPUT)
        x = tt.create_bel(f"S{s}_XOR{side}", "XORCY", z=z + zx)
        tt.add_bel_pin(x, "CI", carry_in, PinType.INPUT)
        tt.add_bel_pin(x, "LI", lut_out, PinType.INPUT)
        tt.add_bel_pin(x, "O", xo, PinType.OUTPUT)
        tt.create_pip(xo, w[out], timing_class="SITE")     # FXMUX / GYMUX = FXOR / GXOR: the sum to X / Y ...
        tt.create_pip(xo, d, timing_class="SITE")          # ... and to the flip-flop


def add_muxes(tt, s, w, pips):
    """The wide multiplexers: F5MUX (I1 the F LUT, I0 the G LUT, S BX) out on the F5 pin, which the graph's PIPs take to
    X and to the FXINA / FXINB of this slice or the one above; FXMUX (I1 FXINA, I0 FXINB, S BY) out on FX, to Y. What the
    FXMUX is follows from the wires reaching its FXINA / FXINB: on the lower slices of a CLB a MUXF6 of two F5s; on X0Y1
    (its FX is FX2) a MUXF7 of the CLB's two F6s; on X1Y1 (FX3) a MUXF8 of the F7s of its CLB and the CLB above."""
    z = s * Z_STRIDE
    m = tt.create_bel(f"S{s}_F5MUX", "MUXF5", z=z + Z_F5MUX)
    tt.add_bel_pin(m, "I0", f"S{s}_GOUT", PinType.INPUT)
    tt.add_bel_pin(m, "I1", f"S{s}_FOUT", PinType.INPUT)
    tt.add_bel_pin(m, "S", w["BX"], PinType.INPUT)
    tt.add_bel_pin(m, "O", w["F5"], PinType.OUTPUT)
    tt.create_pip(w["F5"], w["X"], timing_class="F5X")       # FXMUX = F5 (the graph's own PIP: a route-through)
    tt.create_pip(w["F5"], f"S{s}_DX", timing_class="F5X")   # DXMUX: to the flip-flop
    kind = "MUXF6" if (w["F5"], w["FXINA"]) in pips else {"FX2": "MUXF7", "FX3": "MUXF8"}[w["FX"]]
    m = tt.create_bel(f"S{s}_FXMUX", kind, z=z + Z_FXMUX)
    tt.add_bel_pin(m, "I0", w["FXINB"], PinType.INPUT)
    tt.add_bel_pin(m, "I1", w["FXINA"], PinType.INPUT)
    tt.add_bel_pin(m, "S", w["BY"], PinType.INPUT)
    tt.add_bel_pin(m, "O", w["FX"], PinType.OUTPUT)
    tt.create_pip(w["FX"], w["Y"], timing_class="FXY")       # GYMUX = FX
    tt.create_pip(w["FX"], f"S{s}_DY", timing_class="FXY")   # DYMUX


def add_slicem(tt, s, pins, pips):
    """A SLICEL's BELs, and the LUTs as memories: MEM16F and MEM16G (a RAM's 16 bits or a shift register), on the LUT's
    output wire (a LUT and a memory on one side are told apart by the packer's clusters), its address on the site's pins
    themselves (not permutable: F1..F4 are A0..A3), the write clock on CLK, the write / shift enable on SR, A4 on BX (a
    RAM32X1S's F5 select and write strobe). The data in, MD: on F from BX (DIF_MUX = BX), BY (ALTDIF) or G's last stage
    (SHIFTIN); on G from BY or the SHIFTIN pin. F's last stage goes out on SHIFTOUT, into the lower slice's SHIFTIN."""
    add_slice(tt, s, pins, pips)
    w = {pn: wn for pn, d, wn in pins}
    z = s * Z_STRIDE
    for side, zm in (("F", Z_FMEM), ("G", Z_GMEM)):
        md, q15 = f"S{s}_MD{side}", f"S{s}_{side}Q15"
        tt.create_wire(md, "MEM_D")
        tt.create_wire(q15, "MEM_Q15")
        b = tt.create_bel(f"S{s}_{side}MEM", f"MEM16{side}", z=z + zm)
        for k in range(4):
            tt.add_bel_pin(b, f"A{k}", w[f"{side}{k + 1}"], PinType.INPUT)
        tt.add_bel_pin(b, "A4", w["BX"], PinType.INPUT)
        tt.add_bel_pin(b, "D", md, PinType.INPUT)
        tt.add_bel_pin(b, "CLK", w["CLK"], PinType.INPUT)
        tt.add_bel_pin(b, "SR", w["SR"], PinType.INPUT)
        tt.add_bel_pin(b, "O", f"S{s}_{side}OUT", PinType.OUTPUT)
        tt.add_bel_pin(b, "Q15", q15, PinType.OUTPUT)
    tt.create_pip(w["BX"], f"S{s}_MDF", timing_class="SITE")
    tt.create_pip(w["BY"], f"S{s}_MDF", timing_class="SITE")
    tt.create_pip(f"S{s}_GQ15", f"S{s}_MDF", timing_class="SITE")
    tt.create_pip(w["BY"], f"S{s}_MDG", timing_class="SITE")
    tt.create_pip(w["SHIFTIN"], f"S{s}_MDG", timing_class="SITE")
    tt.create_pip(f"S{s}_FQ15", w["SHIFTOUT"], timing_class="SITE")


def add_iob(tt, s, pins, pips):
    """An IOB / IBUF site: the pad buffer, O (from the fabric through O1, or from the output register: OMUX), T (through
    T1, or from the output-enable register: TMUX), I (to the fabric through the I pin: IMUX, and to the input registers:
    IFFDMUX); the registers (IFF1 / IFF2 on ICLK1 / ICLK2, OFF1 / OFF2 on OTCLK1 / OTCLK2, TFF1 on OTCLK1), which share
    the site's SR and REV. An ODDR2's second flip-flop is OFF2: the pad's multiplexer (OMUX = OFFDDR) reads it inside."""
    w = {pn: wn for pn, d, wn in pins}
    z = s * Z_STRIDE
    pad, i, ffd = f"S{s}_PAD", f"S{s}_IN", f"S{s}_IFFD"
    tt.create_wire(pad, "IO_PAD")
    tt.create_wire(i, "IO_IN")
    tt.create_wire(ffd, "IO_IN")
    tt.create_pip(i, w["I"], timing_class="SITE")
    tt.create_pip(i, ffd, timing_class="SITE")
    b = tt.create_bel(f"S{s}_IOB", "IOB", z=z + Z_IOB)
    tt.add_bel_pin(b, "I", i, PinType.OUTPUT)
    tt.add_bel_pin(b, "PAD", pad, PinType.INOUT)
    def ff(name, typ, zf, d, clk, ce, q):
        f = tt.create_bel(f"S{s}_{name}", typ, z=z + zf)
        tt.add_bel_pin(f, "D", d, PinType.INPUT)
        tt.add_bel_pin(f, "CLK", w[clk], PinType.INPUT)
        tt.add_bel_pin(f, "CE", w[ce], PinType.INPUT)
        tt.add_bel_pin(f, "SR", w["SR"], PinType.INPUT)
        tt.add_bel_pin(f, "Q", q, PinType.OUTPUT)
        return f
    for k, zf in ((1, Z_IFF1), (2, Z_IFF2)):
        tt.add_bel_pin(ff(f"IFF{k}", "IFF", zf, ffd, f"ICLK{k}", "ICE", w[f"IQ{k}"]), "REV", w["REV"], PinType.INPUT)
    if "O1" not in w:                                   # (not on the input-only sites)
        return
    o, t = f"S{s}_O", f"S{s}_T"
    tt.create_wire(o, "IO_OUT")
    tt.create_wire(t, "IO_OUT")
    tt.add_bel_pin(b, "O", o, PinType.INPUT)
    tt.add_bel_pin(b, "T", t, PinType.INPUT)
    tt.create_pip(w["O1"], o, timing_class="SITE")
    tt.create_pip(w["T1"], t, timing_class="SITE")
    for name, typ, zf, d, clk, ce, to in (("OFF1", "OFF", Z_OFF1, "O1", "OTCLK1", "OCE", o), ("OFF2", "OFF", Z_OFF2, "O2", "OTCLK2", "OCE", None),
                                          ("TFF1", "TFF", Z_TFF1, "T1", "OTCLK1", "TCE", t)):
        q = f"S{s}_{name}Q"
        tt.create_wire(q, "IO_OUT")
        ff(name, typ, zf, w[d], clk, ce, q)
        if to:
            tt.create_pip(q, to, timing_class="SITE")


def add_bufgmux(tt, s, pins, pips):
    w = {pn: wn for pn, d, wn in pins}
    b = tt.create_bel(f"S{s}_BUFGMUX", "BUFGMUX", z=s * Z_STRIDE + Z_BUFGMUX)
    tt.add_bel_pin(b, "I0", w["I0"], PinType.INPUT)
    tt.add_bel_pin(b, "O", w["O"], PinType.OUTPUT)


def add_hard(bel_type):
    """A site that is one BEL, its pins the site's: the block RAM (RAMB16), the multiplier (MULT18X18SIO) and the DCM. Not the
    multiplier's cascade (BCIN / BCOUT: the dedicated wires up the column), which the packer refuses."""
    def add(tt, s, pins, pips):
        b = tt.create_bel(f"S{s}_{bel_type}", bel_type, z=s * Z_STRIDE + Z_HARD)
        for pn, d, wn in pins:
            if not pn.startswith(("BCIN", "BCOUT")):
                tt.add_bel_pin(b, pn, wn, PinType.INPUT if d == "i" else PinType.OUTPUT)
    return add


def add_slicel(tt, s, pins, pips):
    """A SLICEL: also a constant 0 on Y, the slice used for nothing else (ISE's and Silinx's GND source: _GND_SOURCE::Y;
    not on a SLICEM, where ISE's bitgen also sets the site's SLICEM bits)."""
    add_slice(tt, s, pins, pips)
    w = {pn: wn for pn, d, wn in pins}
    b = tt.create_bel(f"S{s}_GND", "GNDSRC", z=s * Z_STRIDE + Z_GND)
    tt.add_bel_pin(b, "O", w["Y"], PinType.OUTPUT)


def add_vcc(tt, s, pins, pips):
    """A VCC site: a constant 1 on VCCOUT."""
    w = {pn: wn for pn, d, wn in pins}
    b = tt.create_bel(f"S{s}_VCC", "VCCSRC", z=s * Z_STRIDE)
    tt.add_bel_pin(b, "O", w["VCCOUT"], PinType.OUTPUT)


SITE_BELS = {"SLICEL": add_slicel, "SLICEM": add_slicem, "IOB": add_iob, "DIFFM": add_iob, "DIFFS": add_iob,
             "IBUF": add_iob, "DIFFMI": add_iob, "DIFFSI": add_iob,
             "BUFGMUX": add_bufgmux, "RAMB16": add_hard("RAMB16"), "MULT18X18SIO": add_hard("MULT18X18SIO"), "DCM": add_hard("DCM"),
             "VCC": add_vcc}


def ps(ns):
    return TimingValue(max(0, round(ns * 1000)))


def set_timings(ch, tm):
    """Silinx's model, speed grade -4: the PIPs by the class of the node they drive; the slice's delays."""
    sl, base = tm["slice"], tm["base"]
    for c, d in enumerate(tm["wire"]):
        ch.timing.set_pip_class("4", f"W_{c}", ps(d))
    ch.timing.set_pip_class("4", "SITE", ps(0))
    ch.timing.set_pip_class("4", "BYPASS", ps(sl["Tdick"] - sl["Tdxck"]))   # BX -> flip-flop, beyond the LUT path's setup
    # a LUT input: the LUT and the routed connection's fixed part; the input pin's own offset on the PIP from
    # that pin (all of them raised by the smallest, which the LUT takes: a PIP's delay is not negative)
    low = min(tm["lutPin"])
    for k, d in enumerate(tm["lutPin"]):
        ch.timing.set_pip_class("4", f"LUTPIN{k + 1}", ps(d - low))
    lut = ch.timing.add_cell_variant("4", "LUT4")
    for k in range(4):
        lut.add_comb_arc(f"I{k}", "O", ps(sl["Tilo"] + low + base))
    # the carry, per side (the packer gives a chain's cells the variant of their stage: MUXCYF / MUXCYG, XORCYF / XORCYG;
    # MUXCY / XORCY, F's, before): Silinx's arcs are from the slice's pins to COUT, X, Y and XB (#2286), nextpnr's through
    # the F stage's carry (CYF), which leads on to COUT, Y and XB alike. CIN's ways are kept whole: CIN -> COUT Tbyp (all
    # of it on G's MUXCY), CIN -> X Tcinx, CIN -> Y Tciny, CIN -> XB Tcinxb (the PIP from CYF); an F pin's way to CYF is
    # the mean of what COUT (Topcyf), Y (Topy) and XB (Topxb) ask beyond CIN's, its data input's (DI: pin 1 itself, no
    # LUT, so the connection's fixed part and pin 1's offset here) the mean of COUT's (TopcyfDI) and XB's (TopxbDI).
    # A G pin's: Topcyg, TopcygDI; the XORs' LI: Topx, Togy beyond the LUT's.
    pin1 = base + tm["lutPin"][0]
    to_cyf = (sl["Topcyf"] - sl["Tbyp"] + sl["Topy"] - sl["Tciny"] + sl["Topxb"] - sl["Tcinxb"]) / 3
    di_cyf = (sl["TopcyfDI"] - sl["Tbyp"] + sl["TopxbDI"] - sl["Tcinxb"]) / 2
    ch.timing.set_pip_class("4", "XB", ps(sl["Tcinxb"]))
    for name, ci, s, di in (("MUXCYF", 0, to_cyf - sl["Tilo"], di_cyf + pin1),
                            ("MUXCYG", sl["Tbyp"], sl["Topcyg"] - sl["Tilo"], sl["TopcygDI"] + pin1)):
        for v in (name, "MUXCY") if name == "MUXCYF" else (name,):
            cy = ch.timing.add_cell_variant("4", v)
            cy.add_comb_arc("CI", "O", ps(ci))
            cy.add_comb_arc("S", "O", ps(s))
            cy.add_comb_arc("DI", "O", ps(di))
    for name, ci, li in (("XORCYF", sl["Tcinx"], sl["Topx"] - sl["Tilo"]), ("XORCYG", sl["Tciny"], sl["Togy"] - sl["Tilo"])):
        for v in (name, "XORCY") if name == "XORCYF" else (name,):
            xo = ch.timing.add_cell_variant("4", v)
            xo.add_comb_arc("CI", "O", ps(ci))
            xo.add_comb_arc("LI", "O", ps(li))
    # the wide multiplexers: F1-4 -> F5 Tif5 (beyond the LUT's), F5 -> X the rest of Tif5x; FXINA / FXINB -> FX Tinafx,
    # FX -> Y the rest of Tif6y; their selects BX -> F5 Tbxf5, BY -> FX Tbyfx
    ch.timing.set_pip_class("4", "F5X", ps(sl["Tif5x"] - sl["Tif5"]))
    ch.timing.set_pip_class("4", "FXY", ps(sl["Tif6y"] - sl["Tinafx"]))
    f5 = ch.timing.add_cell_variant("4", "MUXF5")
    f6s = [ch.timing.add_cell_variant("4", t) for t in ("MUXF6", "MUXF7", "MUXF8")]
    for i in ("I0", "I1"):
        f5.add_comb_arc(i, "O", ps(sl["Tif5"] - sl["Tilo"]))
        for f6 in f6s:
            f6.add_comb_arc(i, "O", ps(sl["Tinafx"]))
    f5.add_comb_arc("S", "O", ps(sl["Tbxf5"]))
    for f6 in f6s:
        f6.add_comb_arc("S", "O", ps(sl["Tbyfx"]))
    if "bram" in tm:
        set_hard_timings(ch, tm)
    ff = ch.timing.add_cell_variant("4", "FF")
    ff.add_setup_hold("CLK", "D", ClockEdge.RISING, ps(sl["Tdxck"]), ps(0))
    ff.add_setup_hold("CLK", "CE", ClockEdge.RISING, ps(sl["Tceck"] + base), ps(0))
    ff.add_setup_hold("CLK", "SR", ClockEdge.RISING, ps(sl["Tsrck"] + base), ps(0))
    ff.add_clock_out("CLK", "Q", ClockEdge.RISING, ps(sl["TckoX"]))
    # the SLICEM's memories: Silinx's SPEED_4.mem (a RAM's clock to output Tshcko, a shift register's Treg, MEM16S; a read
    # address through the LUT as a LUT's)
    me = tm.get("mem", {"Treg": 3.762, "Tshcko": 1.956, "TdsY": 0.442, "Tas": 0.522, "Tws": 0.392})
    for name, q in (("MEM16", me["Tshcko"]), ("MEM16S", me["Treg"])):
        v = ch.timing.add_cell_variant("4", name)
        for k in range(4):
            v.add_comb_arc(f"A{k}", "O", ps(sl["Tilo"] + low + base))
            v.add_setup_hold("CLK", f"A{k}", ClockEdge.RISING, ps(me["Tas"]), ps(0))
        v.add_setup_hold("CLK", "D", ClockEdge.RISING, ps(me["TdsY"]), ps(0))
        v.add_setup_hold("CLK", "SR", ClockEdge.RISING, ps(me["Tws"]), ps(0))
        v.add_clock_out("CLK", "O", ClockEdge.RISING, ps(q))
        v.add_clock_out("CLK", "Q15", ClockEdge.RISING, ps(q))
    # the IOB's registers: Silinx's SPEED_4.pad (Tiockiq clock to IQ; Tioock, Tiotck the setup of O / T); its model has none
    # for their enable and set / reset: a slice's. On the falling edge too (IFFN, OFFN: a DDR register's second flip-flop
    # on the inverse of the first's clock)
    pad = tm.get("pad", {"Tiockiq": 0.6, "Tioock": 0.5, "Tiotck": 0.5})
    for name, edge, setup, q in (("IFF", ClockEdge.RISING, 0, pad["Tiockiq"]), ("IFFN", ClockEdge.FALLING, 0, pad["Tiockiq"]),
                                 ("OFF", ClockEdge.RISING, pad["Tioock"], 0), ("OFFN", ClockEdge.FALLING, pad["Tioock"], 0),
                                 ("TFF", ClockEdge.RISING, pad["Tiotck"], 0)):
        v = ch.timing.add_cell_variant("4", name)
        v.add_setup_hold("CLK", "D", edge, ps(setup), ps(0))
        v.add_setup_hold("CLK", "CE", edge, ps(sl["Tceck"] + base), ps(0))
        v.add_setup_hold("CLK", "SR", edge, ps(sl["Tsrck"] + base), ps(0))
        v.add_clock_out("CLK", "Q", edge, ps(q))


def set_hard_timings(ch, tm):
    """The block RAM's and the multiplier's delays (trce's, Silinx's model): a block RAM's clock to DO / DOP and its
    inputs' setups, the same on both ports; a multiplier as its cell's timing variant (the packer's): MULT_COMB A / B ->
    P[j], MULT_PREG the product registered (A / B setup, CLK -> P), MULT_IREG the inputs registered (CLK -> P[j])."""
    br, mu = tm["bram"], tm["mult"]
    r = ch.timing.add_cell_variant("4", "RAMB16")
    for P in "AB":
        clk = f"CLK{P}"
        for i in range(32):
            r.add_clock_out(clk, f"DO{P}{i}", ClockEdge.RISING, ps(br["Tbcko"]))
            r.add_setup_hold(clk, f"DI{P}{i}", ClockEdge.RISING, ps(br["Tbdck"]), ps(0))
        for i in range(4):
            r.add_clock_out(clk, f"DOP{P}{i}", ClockEdge.RISING, ps(br["Tbcko"]))
            r.add_setup_hold(clk, f"DIP{P}{i}", ClockEdge.RISING, ps(br["Tbdck"]), ps(0))
        for i in range(14):
            r.add_setup_hold(clk, f"ADDR{P}{i}", ClockEdge.RISING, ps(br["Tback"]), ps(0))
        for pin, k in (("EN", "Tbeck"), ("WE", "Tbwck"), ("SSR", "Tbrck")):
            r.add_setup_hold(clk, f"{pin}{P}", ClockEdge.RISING, ps(br[k]), ps(0))
    comb = ch.timing.add_cell_variant("4", "MULT_COMB")
    preg = ch.timing.add_cell_variant("4", "MULT_PREG")
    ireg = ch.timing.add_cell_variant("4", "MULT_IREG")
    for j in range(36):
        for i in range(18):
            comb.add_comb_arc(f"A{i}", f"P{j}", ps(mu["Tmult"][j]))
            comb.add_comb_arc(f"B{i}", f"P{j}", ps(mu["Tmult"][j]))
        preg.add_clock_out("CLK", f"P{j}", ClockEdge.RISING, ps(mu["TmsckpP"]))
        ireg.add_clock_out("CLK", f"P{j}", ClockEdge.RISING, ps(mu["Tmsckp"][j]))
    for i in range(18):
        for AB in "AB":
            preg.add_setup_hold("CLK", f"{AB}{i}", ClockEdge.RISING, ps(mu["TmsdckP"]), ps(0))
        ireg.add_setup_hold("CLK", f"A{i}", ClockEdge.RISING, ps(mu["TmsdckA"]), ps(0))
        ireg.add_setup_hold("CLK", f"B{i}", ClockEdge.RISING, ps(mu["TmsdckB"]), ps(0))
    for v in (preg, ireg):
        for pin in ("CEA", "CEB", "CEP"):
            v.add_setup_hold("CLK", pin, ClockEdge.RISING, ps(mu["Tmscck"]), ps(0))
        for pin in ("RSTA", "RSTB", "RSTP"):
            v.add_setup_hold("CLK", pin, ClockEdge.RISING, ps(mu["Tmsrck"]), ps(0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("device")
    ap.add_argument("bba")
    ap.add_argument("--sites")
    ap.add_argument("--unknown-pips")
    ap.add_argument("--timing")
    args = ap.parse_args()
    timing = None
    if args.timing:
        with open(args.timing) as f:
            timing = json.load(f)
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
            classes = timing["pipClass"][str(t)] if timing else None
            for i, (a, b, flags) in enumerate(pips):
                if flags & PIP_ROUTETHRU or i in skip:
                    continue
                tt.create_pip(wires[a], wires[b], timing_class=f"W_{classes[i] // 5}" if classes else "")
            named = {(wires[a], wires[b]) for a, b, flags in pips}
            for s, (stype, pins) in enumerate(sites):
                if stype in SITE_BELS:
                    SITE_BELS[stype](tt, s, pins, named)
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
    if timing:
        set_timings(ch, timing)
    ch.write_bba(args.bba)
    if args.sites:
        with open(args.sites, "w") as f:
            json.dump(site_names, f)
    print(f"{dev.part}: {len(tile_types)} tile types, {len(dev.nodes)} nodes", file=sys.stderr)


if __name__ == "__main__":
    main()
