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

// Spartan-6: the packed slices and the cells of sites of their own as instances of sites, and the netlist's nets as
// nets of their pins, as Silinx's fpga/s6/phys.js makes them (#729). The connections inside a slice are left out: a
// LUT into its position's storage element, carry stage or wide multiplexer, the carry's sum or carry out into the
// position's storage element, the multiplexers' levels. The pins of a slice (UG384):
//   A1 … A6 (B1 … D6)  the inputs of its O6 LUT in their order on A1 …, those of an O6 / O5 pair (5 at most, shared)
//                      on A1 … A5, A6 then tied high; the net a route-through passes after them;
//   AX … DX            the bypass inputs: a storage element's D, the carry's DI or CYINIT (AX), a wide
//                      multiplexer's select (AX, CX: F7AMUX, F7BMUX; BX: F8MUX);
//   CLK, CE, SR        the control set (a SLICEM's RAMs' clock and write enable: CLK, CE); AI … DI their data;
//   CIN, COUT          the carry chain;
//   A … D (O6), AMUX … DMUX, AQ … DQ   the outputs.
// Constant inputs of a LUT are part of its function; the other constant inputs (a bypass input, A6 of an O6 / O5 pair,
// a block RAM's or a DSP's pins tied) are loads of the power nets.

#include <regex>

#include "log.h"
#include "pack.h"

NEXTPNR_NAMESPACE_BEGIN

namespace s6 {

namespace {

bool lut_type(const std::string &t)
{
    static const std::regex re("LUT[1-6]|INV|BUF");
    return std::regex_match(t, re);
}

// the pins of a hard block's cell as the site names them: A<3> -> A3
std::string site_pin_of(const std::string &pin)
{
    static const std::regex re("(.*)<([0-9]+)>");
    std::smatch m;
    return std::regex_match(pin, m, re) ? m[1].str() + m[2].str() : pin;
}

struct Role
{
    int inst;
    std::string r;   // o6, o5, ff, ff5, carry, f7, f8, mem, iob, bufg, bram, dsp
    int p = -1;      // its position
    std::vector<std::string> ps;
};

struct PhysBuilder
{
    const Netlist &nl;
    const Packed &packed;
    Phys out;
    std::vector<std::optional<Role>> role;
    std::vector<std::string> errors;
    pool<std::string> names;

    PhysBuilder(const Netlist &nl, const Packed &packed) : nl(nl), packed(packed), role(nl.cells.size()) {}

    const NCell &cell(int ci) const { return nl.cells.at(ci); }
    static bool is_const(int n) { return n == 0 || n == 1; }
    static int konst(int n) { return is_const(n) ? n : -1; }

    std::string uniq(const std::string &base)
    {
        std::string n = base;
        for (int k = 1; names.count(n); k++)
            n = base + "_" + std::to_string(k);
        names.insert(n);
        return n;
    }

    // the input nets of a LUT on A1 …: its pins I0 … (an INV / BUF: I) in their order
    std::vector<int> lut_input_nets(const NCell &c) const
    {
        static const std::regex re("I[0-9]?");
        std::vector<std::pair<std::string, int>> v;
        for (auto &p : c.pins)
            if (std::regex_match(p.first, re))
                v.push_back(p);
        std::sort(v.begin(), v.end());
        std::vector<int> nets;
        for (auto &p : v)
            nets.push_back(p.second);
        return nets;
    }

    void roles()
    {
        for (int si = 0; si < int(packed.slices.size()); si++) {
            const Slice &s = packed.slices[si];
            int inst = int(out.insts.size());
            auto at = [&](int c, const char *r, int p) {
                if (c >= 0)
                    role[c] = Role{inst, r, p, {}};
            };
            for (int p = 0; p < 4; p++) {
                const Pos &q = s.pos[p];
                if (q.o6.cell()) {
                    const NCell &c = cell(q.o6.v);
                    // (a RAM or shift register: one role for all its positions, D first)
                    if (is_mem(c.type)) {
                        if (!role[q.o6.v])
                            role[q.o6.v] = Role{inst, "mem", p, mem_positions(s, q.o6.v)};
                    } else if (!lut_type(c.type))
                        errors.push_back(c.name + " (" + c.type + "): not a LUT, RAM or shift register");
                    else if (role[q.o6.v])
                        errors.push_back(c.name + ": a cell in two positions of a slice");
                    else
                        at(q.o6.v, "o6", p);
                }
                at(q.o5.cell() ? q.o5.v : -1, "o5", p);
                at(q.ff.cell() ? q.ff.v : -1, "ff", p);
                at(q.ff5.cell() ? q.ff5.v : -1, "ff5", p);
            }
            at(s.carry, "carry", -1);
            at(s.f7[0].cell() ? s.f7[0].v : -1, "f7", 0);
            at(s.f7[1].cell() ? s.f7[1].v : -1, "f7", 2);
            at(s.f8.cell() ? s.f8.v : -1, "f8", 1);
            int named = -1;
            for (int p = 0; p < 4 && named < 0; p++)
                if (s.pos[p].ff.cell())
                    named = s.pos[p].ff.v;
            for (int p = 0; p < 4 && named < 0; p++)
                if (s.pos[p].o6.cell())
                    named = s.pos[p].o6.v;
            if (named < 0)
                named = s.carry;
            Inst i;
            i.name = uniq(named >= 0 ? cell(named).name : "slice" + std::to_string(si));
            i.kind = "slice";
            i.type = std::string("SLICE") + s.kind;
            i.slice = si;
            out.insts.push_back(i);
        }
        static const std::regex no_site("GND|VCC|PULLUP|PULLDOWN");
        static const std::map<std::string, std::pair<std::string, std::string>> own = {
                {"IBUF", {"IOB", "iob"}},       {"IBUFG", {"IOB", "iob"}},           {"OBUF", {"IOB", "iob"}},
                {"OBUFT", {"IOB", "iob"}},      {"IOBUF", {"IOB", "iob"}},           {"BUFG", {"BUFGMUX", "bufg"}},
                {"BUFGMUX", {"BUFGMUX", "bufg"}}, {"RAMB8BWER", {"RAMB8BWER", "bram"}}, {"RAMB16BWER", {"RAMB16BWER", "bram"}},
                {"DSP48A1", {"DSP48A1", "dsp"}}};
        for (int ci = 0; ci < int(nl.cells.size()); ci++) {
            const NCell &c = cell(ci);
            if (role[ci] || std::regex_match(c.type, no_site))
                continue;
            auto o = own.find(c.type);
            if (o == own.end()) {
                errors.push_back(c.name + " (" + c.type + "): the Spartan-6 placer has no site for it yet (#729)");
                continue;
            }
            int inst = int(out.insts.size());
            role[ci] = Role{inst, o->second.second, -1, {}};
            Inst i;
            i.kind = o->second.second;
            i.type = o->second.first;
            i.cell = ci;
            if (i.kind == "iob") {
                int pad = c.type == "IOBUF" ? c.pin("IO", -1) : c.type.rfind("OBUF", 0) == 0 ? c.pin("O", -1) : c.pin("I", -1);
                auto port = c.attrs.find("PORT");
                if (port == c.attrs.end() || pad < 0) {
                    errors.push_back(c.name + ": an I/O buffer not on a port of the top");
                    continue;
                }
                i.port = port->second;
                i.dir = c.type == "IOBUF" ? "inout" : c.type.rfind("OBUF", 0) == 0 ? "out" : "in";
                i.name = uniq(i.port);
            } else
                i.name = uniq(c.name);
            out.insts.push_back(i);
        }
    }

    // ---- the site pin of every cell pin outside its slice (empty: inside it, or a constant set by the slice)
    struct Src
    {
        int p;
        std::string how;
    };
    std::optional<Src> local_source(int inst, int n) const
    {
        int d = nl.nets.at(n).drv_cell;
        if (d < 0 || !role[d] || role[d]->inst != inst)
            return std::nullopt;
        const Role &r = *role[d];
        if (r.r == "carry") {
            static const std::regex re("(O|CO)<([0-9])>");
            std::smatch m;
            const std::string &pin = nl.nets.at(n).drv_pin;
            if (!std::regex_match(pin, m, re))
                return std::nullopt;
            return Src{std::stoi(m[2]), m[1] == "O" ? "xor" : "cy"};
        }
        if (r.r == "o6" || r.r == "o5" || r.r == "f7" || r.r == "f8")
            return Src{r.p, r.r};
        return std::nullopt;
    }
    bool internal(int ci, const std::string &pin, int n) const
    {
        if (!role[ci] || out.insts[role[ci]->inst].kind != "slice")
            return false;
        const Role &r = *role[ci];
        auto src = local_source(r.inst, n);
        if (!src)
            return false;
        if (r.r == "ff")
            return pin == "D" && src->p == r.p;
        if (r.r == "ff5")
            return pin == "D" && src->p == r.p && src->how == "o5";
        if (r.r == "carry") {
            static const std::regex re("(S|DI)<([0-9])>");
            std::smatch m;
            return std::regex_match(pin, m, re) && src->p == std::stoi(m[2]) && src->how == (m[1] == "S" ? "o6" : "o5");
        }
        if (r.r == "f7")
            return (pin == "I0" || pin == "I1") && src->how == "o6" && src->p == (pin == "I0" ? r.p : r.p + 1);
        if (r.r == "f8")
            return (pin == "I0" || pin == "I1") && src->how == "f7";
        return false;
    }

    std::vector<std::array<int, 2>> vcc_a6;      // [inst, position]
    std::vector<std::array<int, 3>> const_x;     // [inst, position, value]

    // the LUT inputs of each position on A1 …: an O6 alone in their order, an O6 / O5 pair shared (A6 high)
    void lut_pins()
    {
        out.lut_pins.resize(packed.slices.size());
        for (int si = 0; si < int(packed.slices.size()); si++) {
            const Slice &s = packed.slices[si];
            for (int p = 0; p < 4; p++) {
                const Pos &q = s.pos[p];
                int o6 = q.o6.cell() && lut_type(cell(q.o6.v).type) ? q.o6.v : -1, o5 = q.o5.cell() ? q.o5.v : -1;
                int t6 = q.o6.k == Lut::THRU ? q.o6.v : -1, t5 = q.o5.k == Lut::THRU ? q.o5.v : -1;
                LutPins m;
                for (int c : {o6, o5})
                    if (c >= 0)
                        for (int n : lut_input_nets(cell(c)))
                            if (!is_const(n) && std::find(m.order.begin(), m.order.end(), n) == m.order.end())
                                m.order.push_back(n);
                for (int n : {t6, t5})
                    if (n >= 0 && std::find(m.order.begin(), m.order.end(), n) == m.order.end())
                        m.order.push_back(n);
                std::string iname = out.insts[si].name;
                if ((o5 >= 0 || t5 >= 0) && m.order.size() > 5)
                    errors.push_back(iname + ": the LUTs of position " + POS[p] + " share more than 5 inputs");
                if (m.order.size() > 6)
                    errors.push_back(iname + ": position " + POS[p] + " has " + std::to_string(m.order.size()) + " LUT inputs");
                auto pin_of = [&](int n) {
                    if (is_const(n))
                        return std::string();
                    int k = int(std::find(m.order.begin(), m.order.end(), n) - m.order.begin());
                    return std::string(1, POS[p]) + std::to_string(k + 1);
                };
                if (o6 >= 0) {
                    m.has_o6 = true;
                    for (int n : lut_input_nets(cell(o6)))
                        m.o6.push_back(pin_of(n));
                }
                if (o5 >= 0) {
                    m.has_o5 = true;
                    for (int n : lut_input_nets(cell(o5)))
                        m.o5.push_back(pin_of(n));
                }
                if (t6 >= 0)
                    m.o6thru = pin_of(t6);
                if (t5 >= 0)
                    m.o5thru = pin_of(t5);
                int di = s.carry >= 0 ? cell(s.carry).pin(std::string("DI<") + char('0' + p) + ">", 0) : -1;
                bool thru = t6 >= 0 && di == t6;
                // a constant DI: O5's (A6 high), or the bypass input's when the O6 LUT takes all six inputs
                if (di >= 0 && is_const(di) && q.o5.none())
                    m.di_const = m.order.size() > 5 ? "x" : "o5";
                if (o5 >= 0 || t5 >= 0 || thru || m.di_const == "o5")
                    vcc_a6.push_back({si, p});
                if (m.di_const == "x")
                    const_x.push_back({si, p, konst(di)});
                m.di_thru = thru;
                out.lut_pins[si][p] = m;
            }
        }
    }

    static const char *ctrl_pin(const std::string &pin)
    {
        if (pin == "C" || pin == "G")
            return "CLK";
        if (pin == "CE" || pin == "GE")
            return "CE";
        if (pin == "R" || pin == "S" || pin == "CLR" || pin == "PRE")
            return "SR";
        return nullptr;
    }

    // the site pins of an input pin of a cell (none: inside its site, or a constant), n its net
    std::vector<std::string> in_pin(int ci, const std::string &pin, int n)
    {
        if (!role[ci])
            return {};
        const Role &r = *role[ci];
        const NCell &c = cell(ci);
        const Inst &inst = out.insts[r.inst];
        auto one = [](const std::string &s) { return s.empty() ? std::vector<std::string>{} : std::vector<std::string>{s}; };
        if (inst.kind != "slice") {
            if (inst.kind == "iob")
                return one(pin == "I" ? "O" : pin == "T" ? "T" : "");
            if (inst.kind == "bufg")
                return one(pin == "I" ? "I0" : pin == "I0" || pin == "I1" || pin == "S" ? pin : "");
            return one(site_pin_of(pin));
        }
        if (internal(ci, pin, n))
            return {};
        const Slice &s = packed.slices[inst.slice];
        const auto &lp = out.lut_pins[r.inst];
        std::string P(1, POS[std::max(r.p, 0)]);
        if (r.r == "mem")
            return mem_in_pins(c, r.ps, pin, konst(n));
        if (r.r == "o6" || r.r == "o5") {
            if (is_const(n))
                return {};
            int k = pin.size() > 1 ? pin[1] - '0' : 0;
            const auto &v = r.r == "o6" ? lp[r.p].o6 : lp[r.p].o5;
            return one(k < int(v.size()) ? v[k] : "");
        }
        if (r.r == "ff" || r.r == "ff5") {
            FfKind kind = *ff_kind(c.type);
            if (pin == "D")
                return one(r.r == "ff5" && s.pos[r.p].o5.k == Lut::THRU && s.pos[r.p].o5.v == n ? lp[r.p].o5thru : P + "X");
            const char *cp = ctrl_pin(pin);
            if (!cp)
                return {};
            std::string scp = cp;
            if (scp == "CE" && konst(n) == 1)
                return {};
            if (scp == "SR" && (konst(n) == 0 || !kind.sr))
                return {};
            if (scp == "CLK" && is_const(n))
                return {};
            return {scp};
        }
        if (r.r == "carry") {
            static const std::regex di_re("DI<([0-9])>"), s_re("S<([0-9])>");
            std::smatch m;
            if (std::regex_match(pin, m, di_re)) {
                int k = std::stoi(m[1]);
                const Pos &q = s.pos[k];
                if (q.o5.k == Lut::THRU && q.o5.v == n)
                    return one(lp[k].o5thru);
                return is_const(n) || (q.o6.k == Lut::THRU && q.o6.v == n) ? std::vector<std::string>{}
                                                                            : std::vector<std::string>{std::string(1, POS[k]) + "X"};
            }
            if (std::regex_match(pin, m, s_re)) {
                int k = std::stoi(m[1]);
                if (is_const(n))
                    return {};
                return s.pos[k].o6.k == Lut::THRU && s.pos[k].o6.v == n ? one(lp[k].o6thru) : std::vector<std::string>{};
            }
            if (pin == "CYINIT")
                return is_const(n) ? std::vector<std::string>{} : std::vector<std::string>{"AX"};
            if (pin == "CI") {
                if (is_const(n))
                    return {};
                int d = nl.nets.at(n).drv_cell;
                if (d >= 0 && cell(d).type == "CARRY4" && nl.nets.at(n).drv_pin == "CO<3>")
                    return {"CIN"};
                errors.push_back(c.name + ": its carry in comes from " + (d >= 0 ? cell(d).name : "no cell") + ", not from a carry chain");
                return {};
            }
            return {};
        }
        if (r.r == "f7")
            return pin == "S" && !is_const(n) ? std::vector<std::string>{r.p == 0 ? "AX" : "CX"} : std::vector<std::string>{};
        if (r.r == "f8")
            return pin == "S" && !is_const(n) ? std::vector<std::string>{"BX"} : std::vector<std::string>{};
        return {};
    }

    // the site pin of an output pin of a cell
    std::string out_pin(int ci, const std::string &pin, int n) const
    {
        if (!role[ci])
            return "";
        const Role &r = *role[ci];
        const NCell &c = cell(ci);
        const Inst &inst = out.insts[r.inst];
        if (inst.kind == "iob")
            return pin == "O" && c.type != "OBUF" && c.type != "OBUFT" ? "I" : "";
        if (inst.kind == "bufg")
            return pin == "O" ? "O" : "";
        if (inst.kind != "slice")
            return site_pin_of(pin);
        const Slice &s = packed.slices[inst.slice];
        std::string P(1, POS[std::max(r.p, 0)]);
        if (r.r == "mem")
            return mem_out_pin(c, r.ps, pin);
        if (r.r == "o6")
            return P;
        if (r.r == "o5" || r.r == "ff5" || r.r == "f7")
            return P + "MUX";
        if (r.r == "ff")
            return P + "Q";
        if (r.r == "f8")
            return "BMUX";
        if (r.r == "carry") {
            static const std::regex re("(O|CO)<([0-9])>");
            std::smatch m;
            if (!std::regex_match(pin, m, re))
                return "";
            int k = std::stoi(m[2]);
            std::string L(1, POS[k]);
            if (m[1] == "O")
                return s.pos[k].ff.k == Ff::AND2L ? L + "Q" : L + "MUX";
            if (k < 3)
                return L + "MUX";
            for (auto &l : nl.nets.at(n).loads)
                if (cell(l.cell).type == "CARRY4" && l.pin == "CI")
                    return "COUT";
            return "DMUX";
        }
        return "";
    }

    void add_load(PNet &net, pool<std::string> &keys, int inst, const std::string &pin)
    {
        std::string k = std::to_string(inst) + ":" + pin;
        if (keys.count(k))
            return;
        keys.insert(k);
        net.loads.push_back({inst, pin});
    }

    void nets()
    {
        PNet vcc, gnd;
        vcc.power = 1;
        gnd.power = 0;
        pool<std::string> vcc_keys, gnd_keys;
        auto power = [&](int v, int inst, const std::string &pin) {
            if (v)
                add_load(vcc, vcc_keys, inst, pin);
            else
                add_load(gnd, gnd_keys, inst, pin);
        };
        for (int n = 2; n < int(nl.nets.size()); n++) {
            const NNet &nn = nl.nets[n];
            if (nn.drv_cell < 0 || !role[nn.drv_cell])
                continue;
            PNet net;
            net.net = n;
            pool<std::string> keys;
            for (auto &l : nn.loads) {
                if (!role[l.cell])
                    continue;
                for (auto &p : in_pin(l.cell, l.pin, n))
                    add_load(net, keys, role[l.cell]->inst, p);
            }
            if (net.loads.empty())
                continue;
            std::string pin = out_pin(nn.drv_cell, nn.drv_pin, n);
            if (pin.empty()) {
                errors.push_back(cell(nn.drv_cell).name + ": its output " + nn.drv_pin + " has no way out of its site");
                continue;
            }
            net.has_driver = true;
            net.driver = {role[nn.drv_cell]->inst, pin};
            out.nets.push_back(net);
        }
        // the constant inputs that are pins: the bypass inputs, A6 of the O6 / O5 pairs, the hard blocks', the IOBs'
        for (int ci = 0; ci < int(nl.cells.size()); ci++) {
            if (!role[ci])
                continue;
            const Role &r = *role[ci];
            const NCell &c = cell(ci);
            const std::string &kind = out.insts[r.inst].kind;
            if (r.r == "mem") {
                for (auto &sp : mem_const_pins(c, r.ps, [](int x) { return konst(x); }))
                    power(sp.second, r.inst, sp.first);
                continue;
            }
            for (auto &pn : c.pins) {
                int n = pn.second;
                const std::string &pin = pn.first;
                if (!is_const(n) || c.is_output(pin))
                    continue;
                std::string sp;
                if (kind == "slice") {
                    const char *cp = ctrl_pin(pin);
                    bool ff = r.r == "ff" || r.r == "ff5";
                    if (ff && pin == "D")
                        sp = std::string(1, POS[r.p]) + "X";
                    else if (ff && cp && std::string(cp) == "CE" && n == 0)
                        sp = "CE";
                    else if (ff && cp && std::string(cp) == "SR" && n == 1 && ff_kind(c.type)->sr)
                        sp = "SR";
                } else if (kind == "iob")
                    sp = pin == "I" ? "O" : "";
                else if (kind == "bram" || kind == "dsp")
                    sp = site_pin_of(pin);
                if (!sp.empty())
                    power(n, r.inst, sp);
            }
        }
        for (auto &a : vcc_a6)
            power(1, a[0], std::string(1, POS[a[1]]) + "6");
        for (auto &x : const_x)
            power(x[2], x[0], std::string(1, POS[x[1]]) + "X");
        // a storage element as a gate (AND2L): its slice's clock and clock enable high, its set / reset low
        for (int si = 0; si < int(packed.slices.size()); si++)
            if (packed.slices[si].ctrl && *packed.slices[si].ctrl == AND2L) {
                power(1, si, "CLK");
                power(1, si, "CE");
                power(0, si, "SR");
            }
        if (!vcc.loads.empty())
            out.nets.push_back(vcc);
        if (!gnd.loads.empty())
            out.nets.push_back(gnd);
        // one net per site pin
        dict<std::string, int> pin_net;
        for (int k = 0; k < int(out.nets.size()); k++) {
            auto &net = out.nets[k];
            std::vector<PinRef> ends = net.loads;
            if (net.has_driver)
                ends.push_back(net.driver);
            for (auto &e : ends) {
                std::string key = std::to_string(e.inst) + ":" + e.pin;
                auto it = pin_net.find(key);
                if (it != pin_net.end() && it->second != k)
                    errors.push_back(out.insts[e.inst].name + ": its pin " + e.pin + " carries two nets");
                pin_net[key] = k;
            }
        }
    }

    Phys run()
    {
        roles();
        lut_pins();
        nets();
        if (!errors.empty()) {
            std::string all;
            pool<std::string> seen;
            for (auto &e : errors)
                if (!seen.count(e)) {
                    seen.insert(e);
                    all += (all.empty() ? "" : "; ") + e;
                }
            log_error("the Spartan-6 design cannot be placed: %s\n", all.c_str());
        }
        out.cell_inst.assign(nl.cells.size(), -1);
        for (int ci = 0; ci < int(nl.cells.size()); ci++)
            if (role[ci])
                out.cell_inst[ci] = role[ci]->inst;
        return std::move(out);
    }
};

} // namespace

Phys phys_design(const Netlist &nl, const Packed &packed) { return PhysBuilder(nl, packed).run(); }

} // namespace s6

NEXTPNR_NAMESPACE_END
