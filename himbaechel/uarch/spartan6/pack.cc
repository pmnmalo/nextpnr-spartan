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

// Spartan-6: the cells of the netlist grouped into slices, as Silinx's packer groups them (fpga/s6/pack.js, which
// this follows step by step; ISE's map packs no LUTs together by default, -lc off, and neither does this).
//
// A slice (UG384) has four positions A … D, each a LUT6 with two outputs (O6; O6 and O5 of five shared inputs, A6
// high), a flip-flop or latch (FF) and a flip-flop (5FF), one multiplexed output (AMUX …); one control set for its
// storage elements. A SLICEL adds the carry chain (CARRY4) and the wide multiplexers (F7AMUX, F7BMUX, F8MUX), a SLICEM
// the LUTs as distributed RAM and shift registers. In order:
//   - the carry chains, a SLICEL a CARRY4, its LUTs those driving S (a DI from an O5 LUT whose only load is the carry,
//     else by the bypass input); the sum and the carry out both out of a position: the sum through the storage element
//     as a gate (map's AND2L); the CYINIT on AX;
//   - the F8s with their F7s and LUTs, then the F7s two by two;
//   - the SLICEM's RAMs and shift registers, by their clock, write enable and write address, D first;
//   - the storage elements: beside the logic that drives their D when the control set allows, else beside a LUT of
//     their own, else with others of their control set (by the bypass input, or a 5FF through O5);
//   - the multiplexed outputs.

#include <regex>

#include "log.h"
#include "pack.h"

NEXTPNR_NAMESPACE_BEGIN

namespace s6 {

std::optional<FfKind> ff_kind(const std::string &type)
{
    static const std::map<std::string, FfKind> kinds = {
            {"FDRE", {"R", false, false, false}},  {"FDSE", {"S", false, false, false}}, {"FDCE", {"CLR", true, false, false}},
            {"FDPE", {"PRE", true, false, false}}, {"FD", {nullptr, false, false, false}}, {"FDE", {nullptr, false, false, false}},
            {"FDR", {"R", false, false, false}},   {"FDS", {"S", false, false, false}},  {"FDC", {"CLR", true, false, false}},
            {"FDP", {"PRE", true, false, false}},  {"LDCE", {"CLR", true, true, false}}, {"LDPE", {"PRE", true, true, false}},
            {"LD", {nullptr, false, true, false}}, {"LDE", {nullptr, false, true, false}}};
    std::string base = type;
    bool neg = false;
    if (base.size() > 2 && base.compare(base.size() - 2, 2, "_1") == 0) {
        base.resize(base.size() - 2);
        neg = true;
    }
    auto it = kinds.find(base);
    if (it == kinds.end())
        return std::nullopt;
    FfKind k = it->second;
    k.neg = neg;
    return k;
}

bool is_lut(const NCell &c)
{
    static const std::regex lut("LUT[1-6]");
    return std::regex_match(c.type, lut) || c.type == "INV" || c.type == "BUF";
}

namespace {

// the SLICEM's cells: the LUT positions they take, their wide multiplexers, their write address pins (a RAM) or none
struct MemKind
{
    int luts;
    const char *addr;
    int n, f7, f8;
    bool srl;
};
const std::map<std::string, MemKind> &mem_kinds()
{
    static const std::map<std::string, MemKind> m = {
            {"RAM32X1S", {1, "A", 5, 0, 0, false}},  {"RAM64X1S", {1, "A", 6, 0, 0, false}},
            {"RAM32X1D", {2, "A", 5, 0, 0, false}},  {"RAM64X1D", {2, "A", 6, 0, 0, false}},
            {"RAM128X1S", {2, "A", 7, 1, 0, false}}, {"RAM128X1D", {4, "A", 7, 2, 0, false}},
            {"RAM256X1S", {4, "A", 8, 2, 1, false}}, {"RAM32M", {4, "ADDRD", 5, 0, 0, false}},
            {"RAM64M", {4, "ADDRD", 6, 0, 0, false}}, {"SRL16E", {1, nullptr, 0, 0, 0, true}},
            {"SRLC32E", {1, nullptr, 0, 0, 0, true}}, {"SRL16", {1, nullptr, 0, 0, 0, true}},
            {"SRLC16E", {1, nullptr, 0, 0, 0, true}}};
    return m;
}

// the cells on sites of their own
bool own_site(const std::string &t)
{
    static const std::regex own("IBUF|IBUFG|OBUF|OBUFT|IOBUF|BUFG|BUFGMUX|BUFGCE|BUFIO2|BUFH|RAMB8BWER|RAMB16BWER|DSP48A1|"
                                "DCM_SP|PLL_BASE|PLL_ADV|BSCAN_SPARTAN6|GND|VCC|PULLUP|PULLDOWN");
    return std::regex_match(t, own);
}

std::string mem_key(const NCell &c)
{
    const MemKind &m = mem_kinds().at(c.type);
    std::string s = m.srl ? "srl:" : "ram:";
    s += std::to_string(c.pin(m.srl ? "CLK" : "WCLK", 0)) + ":" + std::to_string(c.pin(m.srl ? "CE" : "WE", 1)) + ":";
    if (!m.srl)
        for (int i = 0; i < m.n; i++) {
            std::string a = m.addr, p1 = a + "<" + std::to_string(i) + ">", p2 = a + std::to_string(i);
            s += (i ? "," : "") + std::to_string(c.has(p1) ? c.pin(p1) : c.pin(p2, 0));
        }
    return s;
}

} // namespace

bool is_mem(const std::string &type) { return mem_kinds().count(type); }
bool is_srl(const std::string &type) { return mem_kinds().count(type) && mem_kinds().at(type).srl; }

std::vector<int> lut_inputs(const NCell &c)
{
    static const std::regex in("I[0-9]");
    std::vector<int> out;
    for (auto &p : c.pins)
        if (std::regex_match(p.first, in) && p.second > 1 && std::find(out.begin(), out.end(), p.second) == out.end())
            out.push_back(p.second);
    return out;
}

std::string ctrl_key(const NCell &c)
{
    FfKind k = *ff_kind(c.type);
    int clk = c.pin(k.latch ? "G" : "C", 0), ce = c.pin(k.latch ? "GE" : "CE", 1), sr = k.sr ? c.pin(k.sr, 0) : 0;
    return std::to_string(clk) + ":" + (k.neg ? "1" : "0") + ":" + std::to_string(ce) + ":" + std::to_string(sr) + ":" +
           (sr == 0 ? "-" : k.async ? "a" : "s") + ":" + (k.latch ? "L" : "F");
}

namespace {

size_t union_size(std::vector<int> a, const std::vector<int> &b)
{
    for (int x : b)
        if (std::find(a.begin(), a.end(), x) == a.end())
            a.push_back(x);
    return a.size();
}

struct SlicePacker
{
    const Netlist &nl;
    Packed out;
    std::vector<int> where;   // cell -> slice, -1 none

    SlicePacker(const Netlist &nl) : nl(nl), where(nl.cells.size(), -1) {}

    const NCell &cell(int ci) const { return nl.cells.at(ci); }
    const std::vector<NLoad> &loads(int n) const
    {
        static const std::vector<NLoad> none;
        return n >= 0 && n < int(nl.nets.size()) ? nl.nets[n].loads : none;
    }
    int driver(int n) const { return n >= 0 && n < int(nl.nets.size()) ? nl.nets[n].drv_cell : -1; }
    const std::string &driver_pin(int n) const { return nl.nets.at(n).drv_pin; }

    void put(int ci, int si)
    {
        if (where[ci] >= 0)
            log_error("the cell %s packed twice\n", cell(ci).name.c_str());
        where[ci] = si;
    }
    int lut_of(int net) const
    {
        int d = driver(net);
        return d >= 0 && is_lut(cell(d)) && where[d] < 0 ? d : -1;
    }
    int slice(char kind)
    {
        out.slices.emplace_back();
        out.slices.back().kind = kind;
        return int(out.slices.size()) - 1;
    }
    Slice &S(int si) { return out.slices.at(si); }
    // a position's LUT: the cell driving `net` when it is a free LUT, else a route-through of the net
    void place_lut(int si, int p, int net)
    {
        int lc = net > 1 ? lut_of(net) : -1;
        Lut &l = S(si).pos[p].o6;
        if (lc >= 0) {
            l = {Lut::CELL, lc};
            put(lc, si);
        } else if (net > 1)
            l = {Lut::THRU, net};
        else
            l = {Lut::CONST, net};
    }
    static std::string bus(const char *p, int i) { return std::string(p) + "<" + std::to_string(i) + ">"; }

    // ---- carry chains: CARRY4s linked CO<3> -> CI, bottom first; a SLICEL each, its LUTs those driving S
    void pack_carry()
    {
        std::vector<int> carry;
        for (int i = 0; i < int(nl.cells.size()); i++)
            if (cell(i).type == "CARRY4")
                carry.push_back(i);
        dict<int, int> below, above;
        for (int ci : carry) {
            int d = driver(cell(ci).pin("CI", 0));
            if (d >= 0 && cell(d).type == "CARRY4" && driver_pin(cell(ci).pin("CI", 0)) == "CO<3>")
                below[ci] = d;
        }
        for (auto &b : below)
            above[b.second] = b.first;
        for (int bottom : carry) {
            if (below.count(bottom))
                continue;
            std::vector<int> chain;
            for (int ci = bottom;;) {
                int si = slice('L');
                S(si).carry = ci;
                put(ci, si);
                S(si).chain = int(out.chains.size());
                S(si).chain_index = int(chain.size());
                chain.push_back(si);
                const NCell &c = cell(ci);
                for (int i = 0; i < 4; i++) {
                    place_lut(si, i, c.pin(bus("S", i), 0));
                    // DI from the position's O5 (a 5-input LUT sharing the S LUT's inputs, its only load the carry), or
                    // from the bypass input (#2291)
                    int di = c.pin(bus("DI", i), 0);
                    Lut o6 = S(si).pos[i].o6;
                    bool only = di > 1;
                    for (auto &l : loads(di))
                        only &= l.cell == ci;
                    int dl = only ? lut_of(di) : -1;
                    if (dl >= 0 && o6.cell() && union_size(lut_inputs(cell(o6.v)), lut_inputs(cell(dl))) <= 5 &&
                        lut_inputs(cell(dl)).size() <= 5) {
                        S(si).pos[i].o5 = {Lut::CELL, dl};
                        put(dl, si);
                    } else if (di > 1 && (di != c.pin(bus("S", i), 0) || o6.cell()))
                        S(si).pos[i].x = di;
                }
                // the sum and the carry out both out of a position: the sum through the storage element as a gate
                // (map's AND2L), the slice then without any flip-flop
                for (int i = 0; i < 4; i++) {
                    int o = c.pin(bus("O", i), 0), co = c.pin(bus("CO", i), 0);
                    bool sum_out = false, co_out = false;
                    if (o > 1)
                        for (auto &l : loads(o))
                            sum_out |= !(ff_kind(cell(l.cell).type) && l.pin == "D");
                    if (i < 3 && co > 1)
                        for (auto &l : loads(co))
                            co_out |= !(cell(l.cell).type == "CARRY4" && l.pin == "CI");
                    if (sum_out && co_out && !S(si).ctrl) {
                        S(si).pos[i].ff = {Ff::AND2L, 0};
                        S(si).ctrl = AND2L;
                    }
                }
                // CYINIT from AX when it is a signal (a constant is a setting)
                int cy = c.pin("CYINIT", 0);
                if (cy > 1 && !below.count(ci))
                    cyinit_on_ax(S(si), c, cy);
                auto up = above.find(ci);
                if (up == above.end())
                    break;
                ci = up->second;
            }
            out.chains.push_back(chain);
        }
    }

    // a chain's CYINIT on AX; DI<0>, which needed AX too, then through A's O5 as a route-through of its net (#2638)
    void cyinit_on_ax(Slice &s, const NCell &c, int cy)
    {
        Pos &a = s.pos[0];
        int di = a.x;
        if (di >= 0 && di != cy) {
            if (a.o5.none() && (!a.o6.cell() || union_size(lut_inputs(cell(a.o6.v)), {di}) <= 5))
                a.o5 = {Lut::THRU, di};
            else
                out.warnings.push_back("carry chain at " + c.name + ": CYINIT and DI<0> both need the bypass input AX");
        }
        a.x = cy;
    }

    // ---- the wide multiplexers: an F8 with its two F7s and four LUTs, then the F7s two by two (a SLICEL)
    int f7_of(int net) const
    {
        int d = driver(net);
        return d >= 0 && cell(d).type == "MUXF7" && where[d] < 0 ? d : -1;
    }
    void place_f7(int si, int half, int f7)
    {
        int p = half == 0 ? 0 : 2, q = p + 1;
        S(si).f7[half] = {Mux::CELL, f7};
        put(f7, si);
        place_lut(si, p, cell(f7).pin("I0", 0));
        place_lut(si, q, cell(f7).pin("I1", 0));
        int sel = cell(f7).pin("S", 0);
        if (sel > 1)
            S(si).pos[p].x = sel;
    }
    void pack_muxes()
    {
        for (int ci = 0; ci < int(nl.cells.size()); ci++) {
            if (cell(ci).type != "MUXF8" || where[ci] >= 0)
                continue;
            int a = f7_of(cell(ci).pin("I0", 0)), b = f7_of(cell(ci).pin("I1", 0));
            if (a < 0 || b < 0)
                log_error("the MUXF8 %s: its inputs must be two MUXF7s\n", cell(ci).name.c_str());
            int si = slice('L');
            S(si).f8 = {Mux::CELL, ci};
            put(ci, si);
            place_f7(si, 0, a);
            place_f7(si, 1, b);
            int sel = cell(ci).pin("S", 0);
            if (sel > 1)
                S(si).pos[1].x = sel;
        }
        int half = -1;
        for (int ci = 0; ci < int(nl.cells.size()); ci++) {
            if (cell(ci).type != "MUXF7" || where[ci] >= 0)
                continue;
            if (half >= 0 && S(half).pos[2].o6.none()) {
                place_f7(half, 1, ci);
                half = -1;
                continue;
            }
            int si = slice('L');
            place_f7(si, 0, ci);
            half = si;
        }
    }

    // ---- the SLICEM's RAMs and shift registers: by control key, four LUT positions a slice (D first)
    void pack_mems()
    {
        dict<std::string, int> mem_slices;
        for (int ci = 0; ci < int(nl.cells.size()); ci++) {
            auto mk = mem_kinds().find(cell(ci).type);
            if (mk == mem_kinds().end())
                continue;
            const MemKind &m = mk->second;
            std::string key = mem_key(cell(ci));
            // (a shift register's cascade out, Q15 / Q31, leaves by DMUX from position A only, #1153; a cell of two
            // LUTs takes a pair of positions, D and C or B and A)
            bool cascade = false;
            if (m.srl)
                for (const char *q : {"Q15", "Q31"}) {
                    int n = cell(ci).pin(q, 0);
                    cascade |= n > 1 && !loads(n).empty();
                }
            auto room = [&](int s) {
                std::vector<int> r;
                auto free = [&](int p) { return S(s).pos[p].o6.none(); };
                if (cascade) {
                    if (free(0) && S(s).pos[3].mux.empty())
                        r.push_back(0);
                    return r;
                }
                if (m.luts == 2) {
                    if (free(3) && free(2))
                        return std::vector<int>{3, 2};
                    if (free(1) && free(0))
                        return std::vector<int>{1, 0};
                    return r;
                }
                for (int p : {3, 2, 1, 0})
                    if (free(p))
                        r.push_back(p);
                return r;
            };
            auto it = mem_slices.find(key);
            int si = it == mem_slices.end() ? -1 : it->second;
            if (si < 0 || int(room(si).size()) < m.luts) {
                si = slice('M');
                S(si).mem = key;
                mem_slices[key] = si;
            }
            std::vector<int> free = room(si);
            free.resize(std::min<size_t>(free.size(), m.luts));
            auto has = [&](int p) { return std::find(free.begin(), free.end(), p) != free.end(); };
            Slice &s = S(si);
            for (int p : free)
                s.pos[p].o6 = {Lut::CELL, ci};
            put(ci, si);
            if (cascade)
                s.pos[3].mux = "mc31";
            if (m.f7) {
                if (has(0))
                    s.f7[0] = {Mux::MEM, ci};
                if (has(2))
                    s.f7[1] = {Mux::MEM, ci};
            }
            if (m.f8)
                s.f8 = {Mux::MEM, ci};
            if (m.f8)
                s.pos[1].mux = "f8";
            else if (m.f7)
                for (int p : {0, 2})
                    if (has(p))
                        s.pos[p].mux = "f7";
            if (cell(ci).type == "RAM32M")
                for (int p : free)
                    s.pos[p].mux = "o5";
        }
    }

    // ---- the storage elements
    bool fits(const Slice &s, const std::string &key, bool latch) const
    {
        if ((s.ctrl && *s.ctrl != key) || s.mem)
            return false;
        if (latch)
            for (auto &q : s.pos)
                if (!q.ff5.none())
                    return false;
        return true;
    }
    bool fits_ff5(const Slice &s, const std::string &key) const
    {
        return (!s.ctrl || *s.ctrl == key) && !s.mem && !(s.ctrl && s.ctrl->size() >= 2 && s.ctrl->substr(s.ctrl->size() - 2) == ":L");
    }
    void place_ff(int si, int p, int ci, bool five, int x = -1)
    {
        Slice &s = S(si);
        (five ? s.pos[p].ff5 : s.pos[p].ff) = {Ff::CELL, ci};
        put(ci, si);
        s.ctrl = ctrl_key(cell(ci));
        if (x >= 0)
            s.pos[p].x = x;
        if (five)
            s.pos[p].mux = "ff5";
    }
    struct Src
    {
        int si, p;
        std::string how;
    };
    // the position whose logic drives a net, in a slice
    std::optional<Src> local_source(int net) const
    {
        int d = driver(net);
        if (d < 0 || where[d] < 0)
            return std::nullopt;
        int si = where[d];
        const Slice &s = out.slices[si];
        const NCell &c = cell(d);
        if (c.type == "CARRY4") {
            static const std::regex re("(O|CO)<([0-9])>");
            std::smatch m;
            const std::string &pin = driver_pin(net);
            if (!std::regex_match(pin, m, re))
                return std::nullopt;
            return Src{si, std::stoi(m[2]), m[1] == "O" ? "xor" : "cy"};
        }
        if (c.type == "MUXF7")
            return Src{si, s.f7[0].cell() && s.f7[0].v == d ? 0 : 2, "f7"};
        if (c.type == "MUXF8")
            return Src{si, 1, "f8"};
        for (int p = 0; p < 4; p++) {
            if (s.pos[p].o6.cell() && s.pos[p].o6.v == d)
                return Src{si, p, "o6"};
            if (s.pos[p].o5.cell() && s.pos[p].o5.v == d)
                return Src{si, p, "o5"};
        }
        return std::nullopt;
    }
    bool o5_thru(const Pos &q, int net) const
    {
        if (!q.o5.none() || net <= 1)
            return false;
        if (q.o6.none() || q.o6.k == Lut::CONST || q.o6.k == Lut::THRU)
            return true;
        if (!is_lut(cell(q.o6.v)))
            return false;
        return union_size(lut_inputs(cell(q.o6.v)), {net}) <= 5;
    }
    void pack_ffs()
    {
        std::vector<int> ffs, rest;
        for (int i = 0; i < int(nl.cells.size()); i++)
            if (ff_kind(cell(i).type))
                ffs.push_back(i);
        for (int ci : ffs) {
            std::string key = ctrl_key(cell(ci));
            bool latch = ff_kind(cell(ci).type)->latch;
            auto src = local_source(cell(ci).pin("D", 0));
            if (src) {
                Slice &s = S(src->si);
                if (fits(s, key, latch) && s.pos[src->p].ff.none()) {
                    place_ff(src->si, src->p, ci, false);
                    continue;
                }
                // (the 5FF takes O5 or the bypass input only)
                if (src->how == "o5" && !latch && fits_ff5(s, key) && s.pos[src->p].ff5.none() && s.pos[src->p].mux.empty()) {
                    place_ff(src->si, src->p, ci, true);
                    continue;
                }
            }
            rest.push_back(ci);
        }
        // the LUTs not packed yet: a slice of their own with the flip-flops they drive (by control set)
        std::vector<int> loose;
        pool<int> loose_set;
        for (int i = 0; i < int(nl.cells.size()); i++)
            if (is_lut(cell(i)) && where[i] < 0) {
                loose.push_back(i);
                loose_set.insert(i);
            }
        dict<std::string, int> open_x;
        auto lut_slice = [&](const std::string &key) {
            auto it = open_x.find(key);
            int si = it == open_x.end() ? -1 : it->second;
            bool full = si >= 0;
            if (si >= 0)
                for (auto &q : S(si).pos)
                    full &= !q.o6.none();
            if (si < 0 || full) {
                si = slice('X');
                open_x[key] = si;
            }
            return si;
        };
        dict<int, int> ff_of_lut;
        for (int ci : rest) {
            int d = driver(cell(ci).pin("D", 0));
            if (d >= 0 && loose_set.count(d) && !ff_of_lut.count(d))
                ff_of_lut[d] = ci;
        }
        pool<int> placed;
        for (int lc : loose) {
            auto f = ff_of_lut.find(lc);
            int ff = f == ff_of_lut.end() ? -1 : f->second;
            std::string key = ff >= 0 ? ctrl_key(cell(ff)) : "";
            int si = lut_slice(key);
            int p = 0;
            while (!S(si).pos[p].o6.none())
                p++;
            S(si).pos[p].o6 = {Lut::CELL, lc};
            put(lc, si);
            if (ff >= 0 && fits(S(si), key, ff_kind(cell(ff).type)->latch)) {
                place_ff(si, p, ff, false);
                placed.insert(ff);
            }
        }
        // the other storage elements: with others of their control set, the FF by its bypass input, the 5FF through O5
        // as a route-through of a LUT input (#1143)
        for (int ci : rest) {
            if (placed.count(ci))
                continue;
            std::string key = ctrl_key(cell(ci));
            bool latch = ff_kind(cell(ci).type)->latch, done = false;
            int dnet = cell(ci).pin("D", 0);
            for (int si = 0; si < int(out.slices.size()) && !done; si++) {
                if (!fits(S(si), key, latch))
                    continue;
                for (int p = 0; p < 4; p++) {
                    Pos &q = S(si).pos[p];
                    if (q.x >= 0 && q.x != dnet)
                        continue;
                    if (q.ff.none()) {
                        place_ff(si, p, ci, false, dnet);
                        done = true;
                        break;
                    }
                    if (!latch && q.ff5.none() && q.mux.empty() && fits_ff5(S(si), key) && o5_thru(q, dnet)) {
                        place_ff(si, p, ci, true);
                        S(si).pos[p].o5 = {Lut::THRU, dnet};
                        done = true;
                        break;
                    }
                }
            }
            if (!done) {
                int si = slice('X');
                place_ff(si, 0, ci, false, dnet);
            }
        }
    }

    // ---- the multiplexed outputs: what leaves a position by AMUX … (inside a slice: a storage element's D, the
    // carry's CI, DI and S, a wide multiplexer's inputs; a LUT's input is outside)
    bool used_outside(int net, int si) const
    {
        static const std::regex inside("D|CI|DI<[0-9]>|S<[0-9]>|I[01]");
        for (auto &l : loads(net))
            if (where[l.cell] != si || !std::regex_match(l.pin, inside) || is_lut(cell(l.cell)))
                return true;
        return false;
    }
    void set_mux(Slice &s, int p, const std::string &what, const NCell &c)
    {
        if (s.pos[p].mux.empty() || s.pos[p].mux == what) {
            s.pos[p].mux = what;
            return;
        }
        out.warnings.push_back(c.name + ": the output " + POS[p] + "MUX of its slice carries " + s.pos[p].mux + " already (" +
                               what + " needs it too)");
    }
    void pack_outputs()
    {
        for (int si = 0; si < int(out.slices.size()); si++) {
            Slice &s = S(si);
            if (s.carry >= 0) {
                const NCell &c = cell(s.carry);
                for (int i = 0; i < 4; i++) {
                    int o = c.pin(bus("O", i), 0), co = c.pin(bus("CO", i), 0);
                    bool xor_out = o > 1 && used_outside(o, si), cy_out = false;
                    // (CO<3> goes up the column; out of the slice by DMUX when it has other loads)
                    if (co > 1)
                        for (auto &l : loads(co))
                            cy_out |= !(cell(l.cell).type == "CARRY4" && l.pin == "CI") && (where[l.cell] != si || l.pin != "D");
                    if (s.pos[i].ff.k == Ff::AND2L) {
                        set_mux(s, i, "cy", c);
                        continue;
                    }
                    if (xor_out)
                        set_mux(s, i, "xor", c);
                    if (cy_out)
                        set_mux(s, i, "cy", c);
                }
            }
            for (int h = 0; h < 2; h++)
                if (s.f7[h].cell() && used_outside(cell(s.f7[h].v).pin("O", -1), si))
                    set_mux(s, h ? 2 : 0, "f7", cell(s.f7[h].v));
            if (s.f8.cell() && used_outside(cell(s.f8.v).pin("O", -1), si))
                set_mux(s, 1, "f8", cell(s.f8.v));
            for (int p = 0; p < 4; p++)
                if (s.pos[p].o5.cell() && used_outside(cell(s.pos[p].o5.v).pin("O", -1), si) && s.pos[p].mux != "o5")
                    set_mux(s, p, "o5", cell(s.pos[p].o5.v));
        }
    }

    Packed run()
    {
        pack_carry();
        pack_muxes();
        pack_mems();
        pack_ffs();
        pack_outputs();
        // ---- the rest: sites of their own
        for (int ci = 0; ci < int(nl.cells.size()); ci++)
            if (where[ci] < 0 && !own_site(cell(ci).type))
                log_error("the Spartan-6 packer has no place for the cell %s (%s)\n", cell(ci).name.c_str(), cell(ci).type.c_str());
        Packed live;
        std::vector<int> renum(out.slices.size(), -1);
        for (int i = 0; i < int(out.slices.size()); i++) {
            const Slice &s = out.slices[i];
            bool used = s.carry >= 0;
            for (auto &q : s.pos)
                used |= !q.o6.none() || !q.o5.none() || !q.ff.none() || !q.ff5.none();
            if (!used)
                continue;
            renum[i] = int(live.slices.size());
            live.slices.push_back(s);
        }
        for (auto &ch : out.chains) {
            std::vector<int> c;
            for (int i : ch)
                c.push_back(renum[i]);
            live.chains.push_back(c);
        }
        live.warnings = out.warnings;
        return live;
    }
};

} // namespace

Packed pack_slices(const Netlist &nl) { return SlicePacker(nl).run(); }

} // namespace s6

NEXTPNR_NAMESPACE_END
