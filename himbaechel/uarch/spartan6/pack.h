/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  Pedro Maló
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

// Spartan-6: the packer of Yosys's netlist (synth_xilinx -family xc6s) into whole sites, as Silinx's Map packs it
// (its fpga/s6/pack.js, phys.js, phys-mem.js; Silinx #3135). The cells become SLICEX / SLICEL / SLICEM, IOB, BUFGMUX,
// RAMB16BWER / RAMB8BWER and DSP48A1 cells whose ports are the sites' pins, with their timing arcs (TMG, arcs.cc) and
// carry chains (CHAIN_NEXT), as Silinx gave them before; each site cell says which of the netlist's cells it holds
// (SILINX_PACK, see pack.cc), for the settings Silinx writes into the XDL.
//
// The model, as Silinx's: the netlist's cells in their order (the netlist's: nextpnr reads its cells sorted by name),
// its nets by index, 0 and 1 the constants (a net driven by a GND / VCC cell, or none: Yosys's x / z, which the FPGA's
// unused inputs read as 0); a pin of a bus is NAME<i>.

#ifndef SPARTAN6_PACK_H
#define SPARTAN6_PACK_H

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

namespace s6 {

struct NCell
{
    std::string name, type;
    dict<std::string, Property> params;
    std::vector<std::pair<std::string, int>> pins;   // in the cell's order
    pool<std::string> outputs;                       // its output pins
    pool<std::string> attrs_port;                    // (unused)
    dict<std::string, std::string> attrs;            // the string attributes Silinx gives (LOC, IOSTANDARD, PORT…)
    int pin(const std::string &p, int dflt = 0) const
    {
        for (auto &x : pins)
            if (x.first == p)
                return x.second;
        return dflt;
    }
    bool has(const std::string &p) const
    {
        for (auto &x : pins)
            if (x.first == p)
                return true;
        return false;
    }
    bool is_output(const std::string &p) const { return outputs.count(p); }
};

struct NLoad
{
    int cell;
    std::string pin;
};

struct NNet
{
    NetInfo *ni = nullptr;
    int drv_cell = -1;
    std::string drv_pin;
    std::vector<NLoad> loads;
};

struct Netlist
{
    std::vector<NCell> cells;
    std::vector<NNet> nets;   // 0, 1: the constants
};

// ---- the packed slices (pack.cc): a LUT place, a storage element place, a wide multiplexer
struct Lut
{
    enum Kind { NONE, CELL, THRU, CONST } k = NONE;
    int v = 0;   // the cell, the net passed through, the constant
    bool none() const { return k == NONE; }
    bool cell() const { return k == CELL; }
    bool operator==(const Lut &o) const { return k == o.k && v == o.v; }
};
struct Ff
{
    enum Kind { NONE, CELL, AND2L } k = NONE;
    int v = 0;
    bool none() const { return k == NONE; }
    bool cell() const { return k == CELL; }
};
struct Mux
{
    enum Kind { NONE, CELL, MEM } k = NONE;
    int v = 0;
    bool none() const { return k == NONE; }
    bool cell() const { return k == CELL; }
};
struct Pos
{
    Lut o6, o5;
    Ff ff, ff5;
    int x = -1;        // the bypass input's net, -1 none
    std::string mux;   // what leaves by AMUX …: o5, xor, cy, f7, f8, ff5, mc31; empty: none
};
struct Slice
{
    char kind = 'X';
    Pos pos[4];
    int carry = -1;
    Mux f7[2], f8;   // f7[0]: F7AMUX (A, B), f7[1]: F7BMUX (C, D)
    int chain = -1, chain_index = -1;
    std::optional<std::string> ctrl, mem;
};
struct Packed
{
    std::vector<Slice> slices;
    std::vector<std::vector<int>> chains;
    std::vector<std::string> warnings;
};

const char *const POS = "ABCD";

// ---- the cells' kinds
struct FfKind
{
    const char *sr;   // the set / reset pin, nullptr none
    bool async, latch, neg;
};
std::optional<FfKind> ff_kind(const std::string &type);
bool is_lut(const NCell &c);
bool is_mem(const std::string &type);
bool is_srl(const std::string &type);
std::vector<int> lut_inputs(const NCell &c);
std::string ctrl_key(const NCell &c);
const std::string AND2L = "and2l";

Packed pack_slices(const Netlist &nl);

// ---- the physical design (phys.cc)
struct PinRef
{
    int inst;
    std::string pin;
};
struct Inst
{
    std::string name, kind, type;   // kind: slice, iob, bufg, bram, dsp; type: the site cell's
    int slice = -1, cell = -1;
    std::string port, dir;
};
struct PNet
{
    int net = -1;        // the netlist's, -1 a power net
    int power = -1;      // 1 vcc, 0 gnd
    bool has_driver = false;
    PinRef driver;
    std::vector<PinRef> loads;
};
struct LutPins
{
    std::vector<int> order;
    std::vector<std::string> o6, o5;   // the site pin of each input (empty: a constant)
    bool has_o6 = false, has_o5 = false;
    std::string o6thru, o5thru, di_const;
    bool di_thru = false;
};
struct Phys
{
    std::vector<Inst> insts;
    std::vector<PNet> nets;
    std::vector<std::array<LutPins, 4>> lut_pins;   // by slice
    std::vector<int> cell_inst;
};

Phys phys_design(const Netlist &nl, const Packed &packed);

// the memory cells' site pins (phys_mem.cc)
std::vector<std::string> mem_positions(const Slice &s, int ci);
std::vector<std::string> mem_in_pins(const NCell &c, const std::vector<std::string> &ps, const std::string &pin, int konst);
std::vector<std::pair<std::string, int>> mem_const_pins(const NCell &c, const std::vector<std::string> &ps,
                                                       const std::function<int(int)> &konst);
std::string mem_out_pin(const NCell &c, const std::vector<std::string> &ps, const std::string &pin);
int mem_depth(const std::string &type);

// ---- the timing arcs of a site cell (arcs.cc): the TMG parameter, as Silinx's fpga/s6/nextpnr.js timingParam
std::string timing_param(const Netlist &nl, const Packed &packed, const Phys &phys, int inst,
                         const std::vector<std::string> &ins_routed, const pool<std::string> &ins_all,
                         const pool<std::string> &outs_all);

// ---- the whole packer: the netlist's cells replaced by site cells
bool is_site_netlist(Context *ctx);
void pack_netlist(Context *ctx);

} // namespace s6

NEXTPNR_NAMESPACE_END

#endif
