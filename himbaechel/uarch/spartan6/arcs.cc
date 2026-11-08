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

// Spartan-6: a site cell's timing arcs, its TMG parameter (spartan6.cc reads it), as Silinx gave it: its
// fpga/s6/timing-arcs.js on the settings its fpga/s6/xdl.js writes, by trce's arc types (timing_model.h). A slice's:
// each LUT's inputs (those its equation names) to its output and to its output multiplexer (O5 / O6, F7 / F8, the
// carry's XOR or CY: the chain's inputs at and below it, and CIN), a storage element's data by its input multiplexer,
// its CE and SR, a gate (AND2L) passing its data to its output, the carry out; a block RAM's and a DSP48A1's on their
// pins in use. The delays are the model's defaults (an unplaced site, of no site class).

#include <cmath>
#include <regex>

#include "pack.h"
#include "timing_model.h"

NEXTPNR_NAMESPACE_BEGIN

namespace s6 {

namespace {

const char *const LETTERS = "ABCD";

double T(const char *name) { return s6_model::base().at(name); }

// the median of a family of trce's arc types, NAN when none
double family_median(const std::regex &re)
{
    std::vector<double> v;
    for (auto &t : s6_model::types())
        if (std::regex_search(t.first, re))
            v.push_back(t.second);
    std::sort(v.begin(), v.end());
    return v.empty() ? NAN : v[v.size() / 2];
}
double ty(const std::string &name, const char *family, double d)
{
    auto it = s6_model::types().find(name);
    if (it != s6_model::types().end())
        return it->second;
    double m = family_median(std::regex(family));
    return std::isnan(m) ? d : m;
}
// the longest of the types matching re, else d (the hard blocks')
double ty_max(const char *re, double d)
{
    std::regex r(re);
    double best = NAN;
    for (auto &t : s6_model::types())
        if (std::regex_search(t.first, r))
            best = std::isnan(best) ? t.second : std::max(best, t.second);
    return std::isnan(best) ? d : best;
}

// a LUT's value for the inputs I0 …: its INIT (an INV / BUF: its own)
std::vector<int> lut_init(const NCell &c)
{
    if (c.type == "INV")
        return {1, 0};
    if (c.type == "BUF")
        return {0, 1};
    int k = c.type[3] - '0';
    std::vector<int> b(1 << k, 0);
    auto it = c.params.find("INIT");
    if (it != c.params.end())
        for (int i = 0; i < (1 << k) && i < int(it->second.str.size()); i++)
            b[i] = it->second.str[i] == '1';
    return b;
}

struct Arcs
{
    std::vector<std::tuple<std::string, std::string, double>> comb;
    std::vector<std::pair<std::string, double>> setup, clk_to_out;
};

struct SliceCfg
{
    // the settings the arcs read (xdl.js's names): <L>6LUT / <L>5LUT as the pins their equations name, <L>CY0,
    // <L>OUTMUX, <L>6RAMMODE, <L>5FF, <L>FF, <L>FFMUX, CEUSED, SRUSED, COUTUSED, CLKINV
    std::map<std::string, std::string> m;
    std::map<std::string, std::vector<std::string>> lut;
};

// the pins an equation of a LUT names: those of its inputs, none when its value is a constant
std::vector<std::string> eq_pins(const std::vector<int> &init, const std::vector<std::string> &pins, const std::vector<int> &konst)
{
    std::vector<std::string> vars;
    for (auto &p : pins)
        if (!p.empty() && std::find(vars.begin(), vars.end(), p) == vars.end())
            vars.push_back(p);
    std::sort(vars.begin(), vars.end());
    int n = int(vars.size()), ones = 0, zeros = 0;
    for (int v = 0; v < (1 << n); v++) {
        int idx = 0;
        for (int k = 0; k < int(pins.size()); k++) {
            int b = !pins[k].empty() ? (v >> int(std::find(vars.begin(), vars.end(), pins[k]) - vars.begin())) & 1 : konst[k];
            idx |= b << k;
        }
        (idx < int(init.size()) && init[idx] ? ones : zeros)++;
    }
    return ones && zeros ? vars : std::vector<std::string>{};
}

SliceCfg slice_cfg(const Netlist &nl, const Packed &packed, const Phys &phys, int i, const pool<std::string> &drives,
                   const pool<std::string> &takes, const pool<std::string> &a6_high)
{
    SliceCfg cfg;
    const Slice &s = packed.slices[phys.insts[i].slice];
    const auto &lp = phys.lut_pins[i];
    auto cell = [&](int c) -> const NCell & { return nl.cells.at(c); };
    const NCell *carry = s.carry >= 0 ? &cell(s.carry) : nullptr;
    const NCell *ffc = nullptr;
    for (auto &q : s.pos)
        if (!ffc && q.ff.cell())
            ffc = &cell(q.ff.v);
    for (auto &q : s.pos)
        if (!ffc && q.ff5.cell())
            ffc = &cell(q.ff5.v);
    // a LUT's inputs as the equation takes them: their site pins, or the constants they are tied to
    auto equation = [&](const NCell &c, const std::vector<std::string> &pins) {
        static const std::regex re("I[0-9]?");
        std::vector<std::pair<std::string, int>> v;
        for (auto &p : c.pins)
            if (std::regex_match(p.first, re))
                v.push_back(p);
        std::sort(v.begin(), v.end());
        std::vector<std::string> sp;
        std::vector<int> konst;
        for (int k = 0; k < int(v.size()); k++) {
            sp.push_back(k < int(pins.size()) ? pins[k] : "");
            konst.push_back(v[k].second == 1 ? 1 : 0);
        }
        return eq_pins(lut_init(c), sp, konst);
    };
    for (int k = 0; k < 4; k++) {
        const Pos &q = s.pos[k];
        const LutPins &m = lp[k];
        std::string L(1, LETTERS[k]);
        bool high = a6_high.count(std::to_string(i) + ":" + L);
        auto with_high = [&](std::vector<std::string> pins) {
            if (high && std::find(pins.begin(), pins.end(), L + "6") == pins.end())
                pins.push_back(L + "6");
            return pins;
        };
        bool mem_cell = q.o6.cell() && is_mem(cell(q.o6.v).type);
        // the LUTs (a memory's: #RAM, its inputs named by no equation)
        if (mem_cell) {
            cfg.m[L + "6LUT"] = "#RAM";
            const std::string &t = cell(q.o6.v).type;
            if (is_srl(t))
                cfg.m[L + "6RAMMODE"] = t == "SRLC32E" ? "SRL32" : "SRL16";
        } else if (q.o6.cell()) {
            cfg.m[L + "6LUT"] = "#LUT";
            cfg.lut[L + "6LUT"] = with_high(equation(cell(q.o6.v), m.o6));
        } else if (q.o6.k == Lut::THRU) {
            cfg.m[L + "6LUT"] = "#LUT";
            cfg.lut[L + "6LUT"] = with_high({m.o6thru});
        } else if (q.o6.k == Lut::CONST || high) {
            cfg.m[L + "6LUT"] = "#LUT";
            cfg.lut[L + "6LUT"] = with_high({});
        }
        int di = carry ? carry->pin(std::string("DI<") + char('0' + k) + ">", 0) : -1;
        if (!mem_cell) {
            if (q.o5.cell())
                cfg.lut[L + "5LUT"] = equation(cell(q.o5.v), m.o5);
            else if (q.o5.k == Lut::THRU)
                cfg.lut[L + "5LUT"] = {m.o5thru};
            else if (m.di_thru)
                cfg.lut[L + "5LUT"] = {m.o6thru};
            else if (m.di_const == "o5")
                cfg.lut[L + "5LUT"] = {};
        }
        if (carry)
            cfg.m[L + "CY0"] = q.o5.cell() || (q.o5.k == Lut::THRU && q.o5.v == di) || m.di_thru || m.di_const == "o5" ? "O5" : L + "X";
        // the storage elements
        if (q.ff.k == Ff::AND2L) {
            cfg.m[L + "FF"] = "#AND2L";
            cfg.m[L + "FFMUX"] = "XOR";
        } else if (q.ff.cell()) {
            const NCell &c = cell(q.ff.v);
            cfg.m[L + "FF"] = ff_kind(c.type)->latch ? "#LATCH" : "#FF";
            int d = c.pin("D", 0);
            std::string src = L + "X";
            if (q.o6.cell() && cell(q.o6.v).pin("O", -1) == d)
                src = "O6";
            else if (q.o5.cell() && cell(q.o5.v).pin("O", -1) == d)
                src = "O5";
            else if (carry && carry->pin(std::string("O<") + char('0' + k) + ">", -1) == d)
                src = "XOR";
            else if (carry && carry->pin(std::string("CO<") + char('0' + k) + ">", -1) == d)
                src = "CY";
            else if (k == 0 && s.f7[0].cell() && cell(s.f7[0].v).pin("O", -1) == d)
                src = "F7";
            else if (k == 2 && s.f7[1].cell() && cell(s.f7[1].v).pin("O", -1) == d)
                src = "F7";
            else if (k == 1 && s.f8.cell() && cell(s.f8.v).pin("O", -1) == d)
                src = "F8";
            cfg.m[L + "FFMUX"] = src;
        }
        if (q.ff5.cell())
            cfg.m[L + "5FF"] = "";
        // the outputs
        static const std::map<std::string, std::string> mux = {{"o5", "O5"}, {"xor", "XOR"}, {"cy", "CY"}, {"f7", "F7"}, {"f8", "F8"}, {"mc31", "MC31"}};
        if (drives.count(L + "MUX") && !q.mux.empty())
            cfg.m[L + "OUTMUX"] = q.mux == "ff5" ? L + "5Q" : mux.at(q.mux);
    }
    bool and2l = s.ctrl && *s.ctrl == AND2L;
    if (and2l)
        cfg.m["CLKINV"] = "CLK_B";
    else if (ffc)
        cfg.m["CLKINV"] = s.ctrl->size() > 2 && s.ctrl->at(s.ctrl->find(':') + 1) == '1' ? "CLK_B" : "CLK";
    else if (s.mem)
        cfg.m["CLKINV"] = "CLK";
    if (takes.count("CE") && !s.mem)
        cfg.m["CEUSED"] = "0";
    if (takes.count("SR"))
        cfg.m["SRUSED"] = "0";
    if (carry && drives.count("COUT"))
        cfg.m["COUTUSED"] = "0";
    return cfg;
}

// the arcs of a slice from its settings (Silinx's sliceArcs6)
Arcs slice_arcs(const SliceCfg &cfg)
{
    Arcs a;
    auto has = [&](const std::string &k) { return cfg.m.count(k) > 0; };
    auto get = [&](const std::string &k) {
        auto it = cfg.m.find(k);
        return it == cfg.m.end() ? std::string() : it->second;
    };
    auto pins = [&](const std::string &k) {
        auto it = cfg.lut.find(k);
        std::vector<std::string> v = it == cfg.lut.end() ? std::vector<std::string>{} : it->second;
        std::sort(v.begin(), v.end());
        return v;
    };
    auto o6 = [&](int j) { return pins(std::string(1, LETTERS[j]) + "6LUT"); };
    auto o5 = [&](int j) { return pins(std::string(1, LETTERS[j]) + "5LUT"); };
    struct In
    {
        std::string p;
        bool op;   // a LUT pin (else a bypass input)
        int j;
    };
    auto di = [&](int j) {
        std::vector<In> v;
        std::string L(1, LETTERS[j]), cy = get(L + "CY0");
        if (cy == "O5")
            for (auto &p : o5(j))
                v.push_back({p, true, j});
        else if (cy == L + "X")
            v.push_back({L + "X", false, j});
        return v;
    };
    auto chain_in = [&](int k, bool own) {
        std::vector<In> v;
        for (int j = 0; j <= k; j++) {
            for (auto &p : o6(j))
                v.push_back({p, true, j});
            if (j < k || own)
                for (auto &x : di(j))
                    v.push_back(x);
        }
        return v;
    };
    auto l = [](int j) { return std::string(1, "abcd"[j]); };
    // an arc of the chain from an input of stage j to the output of stage k (-1: COUT)
    auto chain_delay = [&](bool op, int j, int k) {
        if (k < 0)
            return op ? ty("Topcy" + l(j), "^Topcy", T("Topcyb")) : ty("T" + l(j) + "xcy", "^T[a-d]xcy$", T("Tbyp"));
        return op ? ty("Top" + l(j) + l(k), "^Top[a-d][a-d]$", T("Topab")) : ty("T" + l(j) + "x" + l(k), "^T[a-d]x[a-d]$", T("Tdick"));
    };
    auto source = [&](const std::string &v, int k) {
        std::vector<std::pair<std::string, double>> out;
        std::string L(1, LETTERS[k]);
        if (v == "O6")
            for (auto &p : o6(k))
                out.push_back({p, T("TiloMux")});
        else if (v == "O5")
            for (auto &p : o5(k))
                out.push_back({p, T("TiloMux")});
        else if (v == "XOR" || v == "CY") {
            for (auto &x : chain_in(k, v == "CY"))
                out.push_back({x.p, chain_delay(x.op, x.j, k)});
            out.push_back({"CIN", ty("Tcin" + l(k), "^Tcin[a-d]$", T("Tcinb"))});
        } else if (v == "F7" || v == "F8") {
            for (int j = 0; j < 4; j++)
                for (auto &p : o6(j))
                    out.push_back({p, T("TiloMux") + T("Tilo")});
            for (int j = 0; j < 4; j++)
                out.push_back({std::string(1, LETTERS[j]) + "X", T("TiloMux") + T("Tilo")});
        } else
            out.push_back({L + "X", T("Tdick")});
        return out;
    };
    bool clocked = false;
    for (int k = 0; k < 4; k++) {
        std::string L(1, LETTERS[k]);
        for (auto &p : o6(k))
            a.comb.push_back({p, L, T("Tilo")});
        std::string om = get(L + "OUTMUX");
        if (om.size() >= 2 && om.substr(om.size() - 2) == "5Q") {
            a.clk_to_out.push_back({L + "MUX", T("Tcko")});
            clocked = true;
        } else if (!om.empty())
            for (auto &x : source(om, k))
                a.comb.push_back({x.first, L + "MUX", x.second});
        // a shift register: its data in before the clock, its output after it
        if (get(L + "6RAMMODE").rfind("SRL", 0) == 0) {
            clocked = true;
            a.setup.push_back({L + "I", T("Tds")});
            a.clk_to_out.push_back({L, T("Tshcko")});
            if (om == "O6")
                a.clk_to_out.push_back({L + "MUX", T("Tshcko")});
        }
        // the second flip-flop: its data the LUT's O5, else the bypass
        if (has(L + "5FF")) {
            clocked = true;
            auto d = o5(k);
            if (d.empty())
                d.push_back(L + "X");
            for (auto &p : d)
                a.setup.push_back({p, p.back() == 'X' ? T("Tdick") : T("Tas")});
        }
        std::string ff = get(L + "FF");
        if (ff.empty())
            continue;
        std::string fm = get(L + "FFMUX");
        auto data = source(fm, k);
        if (ff == "#AND2L" || ff == "#OR2L") {
            for (auto &x : data)
                a.comb.push_back({x.first, L + "Q", T("Tito")});
            if (has("SRUSED"))
                a.comb.push_back({"SR", L + "Q", T("Tito")});
            continue;
        }
        clocked = true;
        a.clk_to_out.push_back({L + "Q", T("Tcko")});
        for (auto &x : data)
            a.setup.push_back({x.first, fm == "XOR" || fm == "CY" ? T("TasXor") + std::max(0.0, x.second - T("TiloMux"))
                                        : fm == "O6" || fm == "O5" ? T("Tas")
                                                                  : T("Tdick")});
    }
    if (has("COUTUSED")) {
        for (auto &x : chain_in(3, true))
            a.comb.push_back({x.p, "COUT", chain_delay(x.op, x.j, -1)});
        a.comb.push_back({"CIN", "COUT", T("Tbyp")});
    }
    if (clocked && has("CEUSED"))
        a.setup.push_back({"CE", T("Tceck")});
    if (clocked && has("SRUSED"))
        a.setup.push_back({"SR", T("Tsrck")});
    return a;
}

// a block RAM's or DSP48A1's arcs on its pins in use (Silinx's hardArcs6)
Arcs hard_arcs(const NCell &c, const std::string &type, const pool<std::string> &ins, const pool<std::string> &outs)
{
    Arcs a;
    static const std::regex do_re("^DO"), bin("^(ADDR|DI|WE|EN|RST|REGCE)"), abd("^[ABD][0-9]"), abdc("^[ABDC][0-9]"), p_re("^P[0-9]");
    if (type == "RAMB8BWER" || type == "RAMB16BWER") {
        for (auto &o : outs)
            if (std::regex_search(o, do_re))
                a.clk_to_out.push_back({o, ty_max("^Trcko_DO", 1.85)});
        for (auto &i : ins)
            if (std::regex_search(i, bin))
                a.setup.push_back({i, ty_max("^Trcck|^Trdck", 0.3)});
    } else if (type == "DSP48A1") {
        auto reg = [&](const char *p) {
            auto it = c.params.find(p);
            return it != c.params.end() && it->second.as_int64() == 1;
        };
        if (reg("MREG")) {
            for (auto &i : ins)
                if (std::regex_search(i, abd))
                    a.setup.push_back({i, ty_max("^Tdspdck_[ABD]_MREG", 3.1)});
            for (auto &o : outs)
                if (std::regex_search(o, p_re))
                    a.clk_to_out.push_back({o, ty_max("^Tdspcko_P_MREG", 1.6)});
        } else if (!reg("PREG"))
            for (auto &i : ins)
                if (std::regex_search(i, abdc))
                    for (auto &o : outs)
                        if (std::regex_search(o, p_re))
                            a.comb.push_back({i, o, ty_max("^Tdspdo_[AB]_P", 4.0)});
    }
    return a;
}

// the clock a pin of a block RAM goes with: its port's, or the only one in use
std::string clock_of_pin(const std::string &pin, const std::vector<std::string> &clocks)
{
    if (clocks.size() < 2)
        return clocks[0];
    static const std::regex a_re("^(ADDRA|DIA|DIPA|DOA|DOPA|WEA|ENA|RSTA|REGCEA)"), clka("^CLKA");
    bool a = std::regex_search(pin, a_re);
    for (auto &c : clocks)
        if (std::regex_search(c, clka) == a)
            return c;
    return clocks[0];
}

std::string ps(double ns) { return std::to_string(int64_t(std::floor(ns * 1000 + 0.5))); }

} // namespace

std::string timing_param(const Netlist &nl, const Packed &packed, const Phys &phys, int i, const std::vector<std::string> &ins_routed,
                         const pool<std::string> &ins_all, const pool<std::string> &outs_all)
{
    const Inst &inst = phys.insts[i];
    if (inst.kind == "iob" || inst.kind == "bufg")
        return "";
    Arcs a;
    bool inverted = false;
    if (inst.kind == "slice") {
        pool<std::string> a6_high;
        for (auto &n : phys.nets)
            if (n.power == 1)
                for (auto &l : n.loads)
                    if (l.pin.size() == 2 && l.pin[1] == '6')
                        a6_high.insert(std::to_string(l.inst) + ":" + l.pin.substr(0, 1));
        SliceCfg cfg = slice_cfg(nl, packed, phys, i, outs_all, ins_all, a6_high);
        a = slice_arcs(cfg);
        inverted = cfg.m.count("CLKINV") && cfg.m.at("CLKINV") == "CLK_B";
    } else
        a = hard_arcs(nl.cells.at(inst.cell), inst.type, ins_all, outs_all);
    static const std::regex clock_pin("CLK|CLKAWRCLK|CLKBRDCLK|CLKA|CLKB");
    std::vector<std::string> clocks, out;
    for (auto &p : ins_routed)
        if (std::regex_match(p, clock_pin))
            clocks.push_back(p);
    for (auto &c : a.comb)
        out.push_back("c " + std::get<0>(c) + " " + std::get<1>(c) + " " + ps(std::get<2>(c)));
    if (!clocks.empty()) {
        for (auto &c : clocks)
            out.push_back("k " + c + (inverted ? " f" : " r"));
        // (a pin's setup, its longest)
        std::vector<std::pair<std::string, double>> setup, q;
        auto put = [](std::vector<std::pair<std::string, double>> &v, const std::string &p, double d) {
            for (auto &x : v)
                if (x.first == p) {
                    x.second = std::max(x.second, d);
                    return;
                }
            v.push_back({p, d});
        };
        for (auto &s : a.setup)
            if (std::find(ins_routed.begin(), ins_routed.end(), s.first) != ins_routed.end())
                put(setup, s.first, s.second);
        for (auto &s : setup)
            out.push_back("s " + s.first + " " + clock_of_pin(s.first, clocks) + " " + ps(s.second));
        for (auto &s : a.clk_to_out)
            put(q, s.first, s.second);
        for (auto &s : q)
            out.push_back("q " + s.first + " " + clock_of_pin(s.first, clocks) + " " + ps(s.second));
    }
    std::string t;
    for (auto &x : out)
        t += (t.empty() ? "" : ";") + x;
    return t;
}

} // namespace s6

NEXTPNR_NAMESPACE_END
