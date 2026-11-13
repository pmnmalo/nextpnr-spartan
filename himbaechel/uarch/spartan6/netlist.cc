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

// Spartan-6: Yosys's netlist read into the packer's model, and its cells replaced by the site cells the packer makes.
//
// Each site cell says what it holds in its attribute SILINX_PACK, entries `key=value` apart by ';' (';', '=' and '%' in
// a name as %3B, %3D, %25): a slice's positions, o6:A … (a cell, `thru`: a route-through of a LUT input, `const:0` /
// `const:1`), o5:A …, ff:A … (a cell or `and2l`, the storage element as a gate), ff5:A …, x:A … (`1`: the bypass input
// AX … carries the position's net), mux:A … (what leaves by AMUX …: o5, xor, cy, f7, f8, ff5, mc31), carry, f7:A, f7:B,
// f8 (a cell, or `mem`: a wide RAM's); another site's, cell (its netlist cell). An IOB keeps its buffer's attributes
// (LOC, IOSTANDARD, DRIVE, SLEW, PULL, PORT); the power nets are $s6_vcc and $s6_gnd.

#include <regex>

#include "log.h"
#include "pack.h"

NEXTPNR_NAMESPACE_BEGIN

namespace s6 {

namespace {

// the site cells Silinx's own Map gives
bool site_type(const std::string &t)
{
    static const std::regex re("SLICEX|SLICEL|SLICEM|IOB|BUFGMUX|RAMB16BWER|RAMB8BWER|DSP48A1|TIEOFF|DCM|PLL_ADV|BSCAN");
    return std::regex_match(t, re);
}

// A[3] (nextpnr's name of a bus's bit) -> A<3> (Silinx's)
std::string bit_name(const std::string &s)
{
    static const std::regex re("(.*)\\[([0-9]+)\\]");
    std::smatch m;
    return std::regex_match(s, m, re) ? m[1].str() + "<" + m[2].str() + ">" : s;
}

std::string escape(const std::string &s)
{
    std::string o;
    for (char c : s)
        o += c == ';' ? "%3B" : c == '=' ? "%3D" : c == '%' ? "%25" : std::string(1, c);
    return o;
}

bool is_iobuf(Context *ctx, const CellInfo *c)
{
    return c->type.in(ctx->id("$nextpnr_ibuf"), ctx->id("$nextpnr_obuf"), ctx->id("$nextpnr_iobuf"));
}

} // namespace

bool is_site_netlist(Context *ctx)
{
    for (auto &cell : ctx->cells)
        if (!site_type(cell.second->type.str(ctx)))
            return false;
    return true;
}

void pack_netlist(Context *ctx)
{
    // ---- the model: the cells by name (the netlist's order as nextpnr reads it), the constants' drivers left out
    std::vector<CellInfo *> order;
    for (auto &cell : ctx->cells)
        order.push_back(cell.second.get());
    std::sort(order.begin(), order.end(), [&](CellInfo *a, CellInfo *b) { return a->name.str(ctx) < b->name.str(ctx); });
    Netlist nl;
    nl.nets.resize(2);
    dict<IdString, int> net_ix;
    pool<IdString> const0, const1;
    for (CellInfo *c : order) {
        if (c->type.in(ctx->id("GND"), ctx->id("VCC")))
            for (auto &p : c->ports)
                if (p.second.type == PORT_OUT && p.second.net)
                    (c->type == ctx->id("VCC") ? const1 : const0).insert(p.second.net->name);
    }
    auto net_of = [&](NetInfo *n) {
        if (!n || const0.count(n->name))
            return 0;
        if (const1.count(n->name))
            return 1;
        auto it = net_ix.find(n->name);
        if (it != net_ix.end())
            return it->second;
        int k = int(nl.nets.size());
        net_ix[n->name] = k;
        nl.nets.emplace_back();
        nl.nets.back().ni = n;
        return k;
    };
    // a port bit's name, from the I/O buffer nextpnr put on it, when Silinx gives none (PORT)
    dict<IdString, std::string> port_of;
    for (CellInfo *c : order)
        if (is_iobuf(ctx, c))
            for (auto &p : c->ports)
                if (p.second.net)
                    port_of[p.second.net->name] = bit_name(c->name.str(ctx));
    std::vector<CellInfo *> model;
    for (CellInfo *c : order) {
        if (is_iobuf(ctx, c) || c->type.in(ctx->id("GND"), ctx->id("VCC")))
            continue;
        model.push_back(c);
        NCell nc;
        nc.name = c->name.str(ctx);
        nc.type = c->type.str(ctx);
        for (auto &p : c->params)
            nc.params[p.first.str(ctx)] = p.second;
        for (auto &a : c->attrs)
            if (a.second.is_string)
                nc.attrs[a.first.str(ctx)] = a.second.as_string();
        for (auto &p : c->ports) {
            std::string pin = bit_name(p.first.str(ctx));
            if (p.second.type == PORT_OUT) {
                nc.outputs.insert(pin);
                nc.outputs.insert(pin.substr(0, pin.find('<')));
                if (!p.second.net)
                    continue;
            }
            nc.pins.push_back({pin, net_of(p.second.net)});
        }
        if (nc.type.find("BUF") != std::string::npos && !nc.attrs.count("PORT"))
            for (auto &p : c->ports)
                if (p.second.net && port_of.count(p.second.net->name) && p.first.in(ctx->id("I"), ctx->id("O"), ctx->id("IO")))
                    if ((nc.type.rfind("OBUF", 0) == 0) == (p.first == ctx->id("O")) || p.first == ctx->id("IO"))
                        nc.attrs["PORT"] = port_of.at(p.second.net->name);
        nl.cells.push_back(nc);
    }
    for (int ci = 0; ci < int(nl.cells.size()); ci++)
        for (auto &p : nl.cells[ci].pins) {
            if (p.second < 2)
                continue;
            NNet &n = nl.nets[p.second];
            if (nl.cells[ci].is_output(p.first)) {
                n.drv_cell = ci;
                n.drv_pin = p.first;
            } else
                n.loads.push_back({ci, p.first});
        }

    // ---- packed, and its sites' pins and arcs
    Packed packed = pack_slices(nl);
    for (auto &w : packed.warnings)
        log_warning("%s\n", w.c_str());
    Phys phys = phys_design(nl, packed);
    std::vector<std::vector<std::string>> ins_routed(phys.insts.size());
    std::vector<pool<std::string>> ins_all(phys.insts.size()), outs_all(phys.insts.size());
    for (auto &n : phys.nets) {
        if (n.has_driver)
            outs_all[n.driver.inst].insert(n.driver.pin);
        for (auto &l : n.loads) {
            ins_all[l.inst].insert(l.pin);
            if (n.has_driver)
                ins_routed[l.inst].push_back(l.pin);
        }
    }
    std::vector<std::string> tmg(phys.insts.size());
    for (int i = 0; i < int(phys.insts.size()); i++)
        tmg[i] = timing_param(nl, packed, phys, i, ins_routed[i], ins_all[i], outs_all[i]);

    // ---- what each site holds
    auto name_of = [&](int ci) { return escape(nl.cells.at(ci).name); };
    std::vector<std::string> pack_attr(phys.insts.size());
    for (int i = 0; i < int(phys.insts.size()); i++) {
        const Inst &inst = phys.insts[i];
        std::string &a = pack_attr[i];
        auto put = [&](const std::string &k, const std::string &v) { a += (a.empty() ? "" : ";") + k + "=" + v; };
        if (inst.kind != "slice") {
            put("cell", name_of(inst.cell));
            continue;
        }
        const Slice &s = packed.slices[inst.slice];
        auto lut = [&](const Lut &l) {
            return l.k == Lut::CELL ? name_of(l.v) : l.k == Lut::THRU ? std::string("thru") : "const:" + std::to_string(l.v);
        };
        for (int p = 0; p < 4; p++) {
            const Pos &q = s.pos[p];
            std::string P(1, POS[p]);
            if (!q.o6.none())
                put("o6:" + P, lut(q.o6));
            if (!q.o5.none())
                put("o5:" + P, lut(q.o5));
            if (!q.ff.none())
                put("ff:" + P, q.ff.k == Ff::AND2L ? "and2l" : name_of(q.ff.v));
            if (q.ff5.cell())
                put("ff5:" + P, name_of(q.ff5.v));
            if (q.x >= 0)
                put("x:" + P, "1");
            if (!q.mux.empty())
                put("mux:" + P, q.mux);
        }
        if (s.carry >= 0)
            put("carry", name_of(s.carry));
        auto mux = [&](const Mux &m) { return m.k == Mux::CELL ? name_of(m.v) : std::string("mem"); };
        if (!s.f7[0].none())
            put("f7:A", mux(s.f7[0]));
        if (!s.f7[1].none())
            put("f7:B", mux(s.f7[1]));
        if (!s.f8.none())
            put("f8", mux(s.f8));
    }

    // ---- the netlist's cells replaced by the site cells
    dict<IdString, dict<IdString, Property>> kept_attrs;
    for (int i = 0; i < int(phys.insts.size()); i++)
        if (phys.insts[i].kind == "iob")
            for (auto &at : model.at(phys.insts[i].cell)->attrs)
                kept_attrs[ctx->id(phys.insts[i].name)][at.first] = at.second;
    std::vector<IdString> dead;
    for (auto &cell : ctx->cells) {
        for (auto &p : cell.second->ports)
            cell.second->disconnectPort(p.first);
        dead.push_back(cell.first);
    }
    for (IdString d : dead)
        ctx->cells.erase(d);
    std::vector<CellInfo *> site(phys.insts.size());
    for (int i = 0; i < int(phys.insts.size()); i++) {
        const Inst &inst = phys.insts[i];
        CellInfo *c = ctx->createCell(ctx->id(inst.name), ctx->id(inst.type));
        site[i] = c;
        if (kept_attrs.count(c->name))
            c->attrs = kept_attrs.at(c->name);
        c->attrs[ctx->id("SILINX_PACK")] = pack_attr[i];
        if (!tmg[i].empty())
            c->params[ctx->id("TMG")] = tmg[i];
    }
    auto port = [&](int inst, const std::string &pin, PortType dir, NetInfo *net) {
        CellInfo *c = site[inst];
        IdString id = ctx->id(pin);
        if (!c->ports.count(id))
            {
            if (dir == PORT_OUT)
                c->addOutput(id);
            else
                c->addInput(id);
        }
        c->connectPort(id, net);
    };
    for (auto &n : phys.nets) {
        NetInfo *ni = n.net >= 0 ? nl.nets[n.net].ni : ctx->createNet(ctx->id(n.power ? "$s6_vcc" : "$s6_gnd"));
        if (n.has_driver)
            port(n.driver.inst, n.driver.pin, PORT_OUT, ni);
        for (auto &l : n.loads)
            port(l.inst, l.pin, PORT_IN, ni);
    }
    for (auto &ch : packed.chains)
        for (size_t k = 0; k + 1 < ch.size(); k++)
            site[ch[k]]->params[ctx->id("CHAIN_NEXT")] = phys.insts[ch[k + 1]].name;
    // (the nets no site pin carries)
    std::vector<IdString> unused;
    for (auto &n : ctx->nets)
        if (!n.second->driver.cell && n.second->users.entries() == 0)
            unused.push_back(n.first);
    for (IdString n : unused)
        ctx->nets.erase(n);
    int counts[3] = {0, 0, 0};
    for (auto &s : packed.slices)
        counts[s.kind == 'X' ? 0 : s.kind == 'L' ? 1 : 2]++;
    log_info("Packed %d slice(s) (%d SLICEX, %d SLICEL, %d SLICEM), %d site(s) in all.\n", int(packed.slices.size()), counts[0], counts[1],
             counts[2], int(phys.insts.size()));
}

} // namespace s6

NEXTPNR_NAMESPACE_END
