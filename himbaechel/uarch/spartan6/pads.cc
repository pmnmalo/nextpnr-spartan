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

// Spartan-6: the pads and the global clock buffers (pads.h).

#include <regex>

#include "himbaechel_api.h"
#include "log.h"
#include "pads.h"

NEXTPNR_NAMESPACE_BEGIN

namespace s6 {

namespace {

// a PIP's flags (gen/s6_gen.py): the route-throughs
constexpr uint32_t FLAG_RT = 2 | 4;

// a tile's type, as Silinx's device graph names it (the chip database's name less its "_<template>[_<variant>]")
std::string tile_type(Context *ctx, int tile)
{
    static const std::regex suffix("_[0-9]+(_[0-9]+)?$");
    std::string t = IdString(chip_tile_info(ctx->chip_info, tile).type_name).str(ctx);
    return std::regex_replace(t, suffix, "");
}

} // namespace

int vcco_of(const std::string &std)
{
    static const std::vector<std::pair<int, std::regex>> table = {
            {33, std::regex("LVTTL|LVCMOS33|PCI33_3|PCI66_3")},
            {25, std::regex("LVCMOS25|LVDS_25|BLVDS_25|LVPECL_25|RSDS_25|MINI_LVDS_25|SSTL2_I|SSTL2_II|DIFF_SSTL2_I|DIFF_SSTL2_II")},
            {18, std::regex("LVCMOS18|SSTL18_I|DIFF_SSTL18_I|HSTL_I_18|HSTL_III_18|DIFF_HSTL_I_18|DIFF_HSTL_III_18")},
            {15, std::regex("LVCMOS15|HSTL_I|HSTL_III")},
            {12, std::regex("LVCMOS12")}};
    std::string u = std;
    for (auto &ch : u)
        ch = toupper(ch);
    for (auto &e : table)
        if (std::regex_match(u, e.second))
            return e.first;
    return 0;
}

std::string iostandard(Context *ctx, const CellInfo *iob)
{
    auto a = iob->attrs.find(ctx->id("IOSTANDARD"));
    return a == iob->attrs.end() ? "LVCMOS25" : a->second.as_string();
}

void Pads::init(Context *ctx)
{
    // tiles of the general interconnect and of the logic: a dedicated clock path from a pad runs through none
    static const std::regex general_tile("^(INT|IOI_INT|INT_BRAM|INT_TERM|CLEX|IOI$|LIOI|RIOI|TIOI|BIOI)");
    dict<WireId, BelId> buf_in;
    IdString bufgmux = ctx->id("BUFGMUX"), iob = ctx->id("IOB");
    for (BelId b : ctx->getBels())
        if (ctx->getBelType(b) == bufgmux) {
            bufg_order.push_back(b);
            buf_in[ctx->getBelPinWire(b, ctx->id("I0"))] = b;
        }
    pool<BelId> bonded_pads;
    for (const auto &pad : ctx->package_info->pads)
        bonded_pads.insert(ctx->getBelByName(IdStringList::concat(IdString(pad.tile), IdString(pad.bel))));
    for (BelId b : ctx->getBels()) {
        if (ctx->getBelType(b) != iob || !bonded_pads.count(b))
            continue;
        pads.push_back(b);
        char side = tile_type(ctx, b.tile)[0];
        bank[b] = side == 'T' ? 0 : side == 'R' ? 1 : side == 'B' ? 2 : side == 'L' ? 3 : -1;
        // the buffers the pad's input reaches by the dedicated paths: 14 switches at most, no route-through, no
        // switch in a tile of the general interconnect or of the logic
        std::vector<BelId> hits;
        WireId start = ctx->getBelPinWire(b, ctx->id("I"));
        if (start != WireId()) {
            pool<WireId> seen{start};
            std::vector<WireId> q{start};
            for (int depth = 0; depth < 14 && !q.empty(); depth++) {
                std::vector<WireId> next;
                for (WireId w : q)
                    for (PipId p : ctx->getPipsDownhill(w)) {
                        WireId to = ctx->getPipDstWire(p);
                        if ((chip_pip_info(ctx->chip_info, p).flags & FLAG_RT) || seen.count(to) ||
                            std::regex_search(tile_type(ctx, p.tile), general_tile))
                            continue;
                        seen.insert(to);
                        auto hit = buf_in.find(to);
                        if (hit != buf_in.end() && std::find(hits.begin(), hits.end(), hit->second) == hits.end())
                            hits.push_back(hit->second);
                        next.push_back(to);
                    }
                q = next;
            }
        }
        bufgs[b] = hits;
    }
}

void place_pads(Context *ctx, const Pads &pads)
{
    IdString id_BEL = ctx->id("BEL"), id_LOC = ctx->id("LOC"), id_IOB = ctx->id("IOB"), id_BUFGMUX = ctx->id("BUFGMUX");
    dict<BelId, CellInfo *> owner;
    dict<int, std::pair<int, int>> bank_v;   // bank -> its VCCO, its I/Os
    auto bank_ok = [&](BelId pad, int v) { return v == 0 || !bank_v.count(pads.bank.at(pad)) || bank_v.at(pads.bank.at(pad)).first == v; };
    auto place_pad = [&](CellInfo *ci, BelId pad) {
        owner[pad] = ci;
        ci->attrs[id_BEL] = std::string(ctx->nameOfBel(pad));
        auto &e = bank_v[pads.bank.at(pad)];
        if (e.second++ == 0)
            e.first = vcco_of(iostandard(ctx, ci));
    };
    // (--verbose: the pads, for the tests)
    for (BelId b : pads.pads) {
        if (ctx->verbose) {
            std::string l;
            for (BelId h : pads.bufgs.at(b))
                l += std::string(" ") + ctx->nameOfBel(h);
            log_info("pad %s (%s): bank %d, global buffers:%s\n", IdString(ctx->get_bel_package_pin(b)->package_pin).c_str(ctx), ctx->nameOfBel(b),
                     pads.bank.at(b), l.c_str());
        }
    }
    auto volts = [](int v) { return stringf("%d.%d V", v / 10, v % 10); };
    // the cells in their order (the packer's: by name)
    std::vector<CellInfo *> cells;
    for (auto &c : ctx->cells)
        cells.push_back(c.second.get());
    std::sort(cells.begin(), cells.end(), [&](CellInfo *a, CellInfo *b) { return a->name.str(ctx) < b->name.str(ctx); });
    for (CellInfo *ci : cells) {
        if (ci->type != id_IOB || !ci->attrs.count(id_LOC))
            continue;
        std::string loc = ci->attrs.at(id_LOC).as_string();
        BelId pad = ctx->get_package_pin_bel(ctx->id(loc));
        if (pad == BelId() || !pads.bonded(pad))
            log_error("place: %s: LOC %s is not a bonded pad of this device\n", ctx->nameOf(ci), loc.c_str());
        if (owner.count(pad))
            log_error("place: two ports on pad %s (%s, %s)\n", loc.c_str(), ctx->nameOf(owner.at(pad)), ctx->nameOf(ci));
        int v = vcco_of(iostandard(ctx, ci));
        if (!bank_ok(pad, v))
            log_error("place: %s: pad %s is in bank %d, whose other I/Os need VCCO %s, not %s (I/O standards)\n", ctx->nameOf(ci), loc.c_str(),
                      pads.bank.at(pad), volts(bank_v.at(pads.bank.at(pad)).first).c_str(), volts(v).c_str());
        place_pad(ci, pad);
    }
    auto pin_name = [&](BelId pad) {
        const PadInfoPOD *p = ctx->get_bel_package_pin(pad);
        return p ? IdString(p->package_pin).str(ctx) : std::string(ctx->nameOfBel(pad));
    };
    // the global buffers, in the packer's order; a buffer driven by a pad on one its dedicated path reaches
    std::vector<CellInfo *> bufgs;
    for (CellInfo *ci : cells)
        if (ci->type == id_BUFGMUX)
            bufgs.push_back(ci);
    if (bufgs.size() > pads.bufg_order.size())
        log_error("place: %d global clock buffers, the device has %d\n", int(bufgs.size()), int(pads.bufg_order.size()));
    pool<BelId> taken;
    for (CellInfo *ci : bufgs) {
        if (ci->attrs.count(id_BEL)) {
            BelId b = ctx->getBelByNameStr(ci->attrs.at(id_BEL).as_string());
            if (b != BelId())
                taken.insert(b);
        }
    }
    for (CellInfo *ci : bufgs) {
        if (ci->attrs.count(id_BEL))
            continue;
        NetInfo *in = ci->getPort(ctx->id("I0"));
        CellInfo *pad_cell = in && in->driver.cell && in->driver.cell->type == id_IOB ? in->driver.cell : nullptr;
        if (pad_cell && !pad_cell->attrs.count(id_BEL)) {
            // a clock input without a LOC: a free clock-capable pad whose buffer is free
            int v = vcco_of(iostandard(ctx, pad_cell));
            for (BelId p : pads.pads) {
                if (owner.count(p) || !bank_ok(p, v))
                    continue;
                bool free = false;
                for (BelId b : pads.bufgs.at(p))
                    free |= !taken.count(b);
                if (free) {
                    place_pad(pad_cell, p);
                    break;
                }
            }
        }
        BelId pad = pad_cell && pad_cell->attrs.count(id_BEL) ? ctx->getBelByNameStr(pad_cell->attrs.at(id_BEL).as_string()) : BelId();
        BelId b;
        if (pad != BelId())
            for (BelId x : pads.bufgs.at(pad))
                if (b == BelId() && !taken.count(x))
                    b = x;
        if (b == BelId()) {
            for (BelId x : pads.bufg_order)
                if (b == BelId() && !taken.count(x))
                    b = x;
            if (pad != BelId())
                log_warning("%s: the clock enters on pad %s, %s: its global buffer %s is reached through general routing\n", ctx->nameOf(pad_cell),
                            pin_name(pad).c_str(), pads.bufgs.at(pad).empty() ? "not a clock-capable pin (GCLK)" : "whose global buffer is taken",
                            ctx->nameOfBel(b));
        }
        taken.insert(b);
        ci->attrs[id_BEL] = std::string(ctx->nameOfBel(b));
    }
}

} // namespace s6

NEXTPNR_NAMESPACE_END
