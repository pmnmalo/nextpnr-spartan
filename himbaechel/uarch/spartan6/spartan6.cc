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

// Spartan-6: places and routes a design of whole sites. Silinx packs the netlist (LUT6s, CARRY4s, flip-flops, F7 / F8,
// SLICEM's memories, block RAMs, DSP48A1s) into SLICEX / SLICEL / SLICEM, IOB, BUFGMUX, RAMB16BWER / RAMB8BWER, DSP48A1
// and TIEOFF cells, whose ports are the sites' pins, and writes their settings itself once they are placed and routed
// (its fpga/s6/pack.js, phys.js, xdl.js). The chip database has one BEL per site (gen/s6_gen.py).
//
// A cell's parameters, from Silinx:
//   TMG: its timing arcs, ';' between them: 'c from to ps' (combinational), 's pin clock ps' (setup), 'q pin clock ps'
//     (clock to out), 'k clock r|f' (a clock pin and its active edge);
//   CHAIN_NEXT: the cell above it in a chain (a carry chain: COUT -> CIN; a DSP48A1 cascade: PCOUT0 -> PCIN0);
//   GENERAL_OK: a pad's buffer whose clock may leave the dedicated paths (a pad no dedicated path leaves).
// Its attribute BEL fixes it there; with --no-place, all of them do.

#include <regex>
#include <sstream>

#include "log.h"
#include "nextpnr.h"
#include "util.h"

#include "himbaechel_helpers.h"

#define GEN_INIT_CONSTIDS
#define HIMBAECHEL_CONSTIDS "uarch/spartan6/constids.inc"
#define HIMBAECHEL_UARCH spartan6
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

// a PIP's flags (gen/s6_gen.py)
constexpr uint32_t FLAG_GENERAL = 1, FLAG_RT_FREE = 2, FLAG_RT_OWN = 4;
constexpr int FLAG_SITE_SHIFT = 8;

struct CellTiming
{
    dict<std::pair<IdString, IdString>, delay_t> comb;
    dict<IdString, std::pair<IdString, delay_t>> setup, clock_out;
    dict<IdString, bool> clocks;   // the clock pins: rising?
    pool<IdString> comb_in, comb_out;
};

struct Spartan6Impl : HimbaechelAPI
{
    std::string device;
    HimbaechelHelpers h;
    dict<IdString, CellTiming> timing;
    dict<IdString, std::vector<CellInfo *>> chains;   // by the cluster's root, bottom first
    mutable dict<BelId, BelId> next_cache;
    pool<IdString> restricted;                        // the nets kept off the general interconnect
    dict<std::tuple<int, int, int>, std::pair<NetInfo *, int>> rt_use;   // a route-through's site -> its net, its PIPs

    Spartan6Impl(const std::string &device) : device(device) {}
    ~Spartan6Impl() {};

    po::options_description getUArchOptions() override
    {
        po::options_description specific("Spartan-6 specific options");
        return specific;
    }

    void init_database(Arch *arch) override
    {
        init_uarch_constids(arch);
        // xc6slx16csg324-3: the part's database, its package, its speed grade
        std::smatch m;
        if (!std::regex_match(device, m, std::regex("(xc6slx[0-9]+)([a-z]+[0-9]+)(?:-([0-9]))?")))
            log_error("Unknown Spartan-6 device '%s' (e.g. xc6slx16csg324-3).\n", device.c_str());
        arch->load_chipdb(stringf("spartan6/chipdb-%s.bin", m[1].str().c_str()));
        std::string pkg = m[2].str();
        for (auto &c : pkg)
            c = toupper(c);
        arch->set_package(pkg);
        arch->set_speed_grade("3");
    }

    void init(Context *ctx) override
    {
        h.init(ctx);
        HimbaechelAPI::init(ctx);
    }

    // ------------------------------------------------------------------ the design
    void read_timing(CellInfo *ci)
    {
        auto it = ci->params.find(id_TMG);
        if (it == ci->params.end())
            return;
        std::istringstream arcs(it->second.as_string());
        std::string arc;
        CellTiming &t = timing[ci->name];
        while (std::getline(arcs, arc, ';')) {
            std::istringstream f(arc);
            std::string kind, a, b;
            int d = 0;
            f >> kind >> a >> b >> d;
            if (kind == "c") {
                t.comb[std::make_pair(ctx->id(a), ctx->id(b))] = d;
                t.comb_in.insert(ctx->id(a));
                t.comb_out.insert(ctx->id(b));
            } else if (kind == "s")
                t.setup[ctx->id(a)] = std::make_pair(ctx->id(b), delay_t(d));
            else if (kind == "q")
                t.clock_out[ctx->id(a)] = std::make_pair(ctx->id(b), delay_t(d));
            else if (kind == "k")
                t.clocks[ctx->id(a)] = b == "r";
            else if (!kind.empty())
                log_error("%s: the timing arc '%s'\n", ctx->nameOf(ci), arc.c_str());
        }
        ci->params.erase(id_TMG);
    }

    // the cell a chain's CHAIN_NEXT names (a string parameter: its trailing space kept)
    IdString chain_next(const Property &p) const
    {
        std::string s = p.as_string();
        if (!s.empty() && s.back() == ' ')
            s.pop_back();
        return ctx->id(s);
    }

    void pack() override
    {
        dict<IdString, CellInfo *> below;
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            read_timing(ci);
            auto bel = ci->attrs.find(id_BEL);
            if (bel != ci->attrs.end()) {
                BelId b = ctx->getBelByNameStr(bel->second.as_string());
                if (b == BelId() || !isValidBelForCellType(ci->type, b))
                    log_error("%s (%s): cannot be on '%s'.\n", ctx->nameOf(ci), ci->type.c_str(ctx), bel->second.as_string().c_str());
                ctx->bindBel(b, ci, STRENGTH_USER);
                ci->attrs.erase(id_BEL);
            }
            auto next = ci->params.find(id_CHAIN_NEXT);
            if (next != ci->params.end())
                below[chain_next(next->second)] = ci;
        }
        // the chains: clusters whose places follow the dedicated wires (getClusterPlacement)
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (!ci->params.count(id_CHAIN_NEXT) || below.count(ci->name))
                continue;
            std::vector<CellInfo *> chain;
            for (CellInfo *c = ci; c;) {
                chain.push_back(c);
                auto next = c->params.find(id_CHAIN_NEXT);
                c = next == c->params.end() ? nullptr : ctx->cells.at(chain_next(next->second)).get();
            }
            ci->cluster = ci->name;
            ci->constr_abs_z = false;
            for (size_t k = 1; k < chain.size(); k++) {
                CellInfo *c = chain[k];
                ci->constr_children.push_back(c);
                c->cluster = ci->name;
                // (an estimate for the placer's spreading: one a tile up the column)
                c->constr_x = 0;
                c->constr_y = -int(k);
                c->constr_z = 0;
                c->constr_abs_z = false;
            }
            chains[ci->name] = chain;
        }
        if (!chains.empty())
            log_info("%d chain(s) of sites.\n", int(chains.size()));
    }

    // ------------------------------------------------------------------ the sites a cell fits
    // a SLICEX cell fits any slice, a SLICEL one a SLICEL or a SLICEM, a SLICEM one a SLICEM
    bool isValidBelForCellType(IdString cell_type, BelId bel) const override
    {
        IdString b = ctx->getBelType(bel);
        if (cell_type == id_SLICEX)
            return b.in(id_SLICEX, id_SLICEL, id_SLICEM);
        if (cell_type == id_SLICEL)
            return b.in(id_SLICEL, id_SLICEM);
        return b == cell_type;
    }

    BelBucketId getBelBucketForBel(BelId bel) const override
    {
        IdString b = ctx->getBelType(bel);
        return b.in(id_SLICEX, id_SLICEL, id_SLICEM) ? id_SLICE : b;
    }

    BelBucketId getBelBucketForCellType(IdString cell_type) const override
    {
        return cell_type.in(id_SLICEX, id_SLICEL, id_SLICEM) ? id_SLICE : cell_type;
    }

    // a block RAM tile: one RAMB16BWER or two RAMB8BWER
    bool isBelLocationValid(BelId bel, bool explain_invalid) const override
    {
        IdString type = ctx->getBelType(bel);
        if (!type.in(id_RAMB16BWER, id_RAMB8BWER) || !ctx->getBoundBelCell(bel))
            return true;
        Loc l = ctx->getBelLocation(bel);
        for (BelId other : ctx->getBelsByTile(l.x, l.y))
            if (ctx->getBelType(other).in(id_RAMB16BWER, id_RAMB8BWER) && ctx->getBelType(other) != type && ctx->getBoundBelCell(other)) {
                if (explain_invalid)
                    log_nonfatal_error("%s: a block RAM tile takes a RAMB16BWER or RAMB8BWERs\n", ctx->nameOf(ctx->getBoundBelCell(bel)));
                return false;
            }
        return true;
    }

    // the site a chain's next stage is on: the BEL of the same type family whose `in` pin `out` reaches
    BelId next_in_chain(BelId bel, IdString out, IdString in) const
    {
        auto it = next_cache.find(bel);
        if (it != next_cache.end())
            return it->second;
        BelId found;
        auto pin_at = [&](WireId w) {
            for (auto bp : ctx->getWireBelPins(w))
                if (bp.pin == in)
                    found = bp.bel;
        };
        WireId o = ctx->getBelPinWire(bel, out);
        if (o != WireId()) {
            pin_at(o);
            for (PipId p : ctx->getPipsDownhill(o))
                if (found == BelId())
                    pin_at(ctx->getPipDstWire(p));
        }
        next_cache[bel] = found;
        return found;
    }

    bool getClusterPlacement(ClusterId cluster, BelId root_bel, std::vector<std::pair<CellInfo *, BelId>> &placement) const override
    {
        auto it = chains.find(cluster);
        if (it == chains.end())
            return HimbaechelAPI::getClusterPlacement(cluster, root_bel, placement);
        placement.clear();
        BelId b = root_bel;
        for (CellInfo *c : it->second) {
            if (b == BelId() || !isValidBelForCellType(c->type, b))
                return false;
            placement.emplace_back(c, b);
            b = c->type == id_DSP48A1 ? next_in_chain(b, id_PCOUT0, id_PCIN0) : next_in_chain(b, id_COUT, id_CIN);
        }
        return true;
    }

    // ------------------------------------------------------------------ routing
    // The global clocks run on the global network and a pad into its global buffer on its dedicated path, unless a sink
    // is on the general interconnect (a clock read by a LUT) or Silinx says the pad has none.
    bool on_general(WireId w) const
    {
        for (PipId p : ctx->getPipsUphill(w))
            if (chip_pip_info(ctx->chip_info, p).flags & FLAG_GENERAL)
                return true;
        return false;
    }

    void preRoute() override
    {
        for (auto &n : ctx->nets) {
            NetInfo *ni = n.second.get();
            CellInfo *d = ni->driver.cell;
            if (!d || ni->users.entries() == 0)
                continue;
            bool pad = d->type == id_IOB && !d->params.count(id_GENERAL_OK);
            if (d->type != id_BUFGMUX && !pad)
                continue;
            bool keep = true;
            for (auto &usr : ni->users) {
                if (pad && usr.cell->type != id_BUFGMUX)
                    keep = false;
                for (WireId w : ctx->getNetinfoSinkWires(ni, usr))
                    if (d->type == id_BUFGMUX && on_general(w))
                        keep = false;
            }
            if (keep)
                restricted.insert(ni->name);
        }
        log_info("%d net(s) on the global network or dedicated paths only.\n", int(restricted.size()));
    }

    std::tuple<int, int, int> rt_site(PipId pip) const
    {
        Loc l = ctx->getPipLocation(pip);
        return std::make_tuple(l.x, l.y, int(chip_pip_info(ctx->chip_info, pip).flags >> FLAG_SITE_SHIFT));
    }

    bool checkPipAvailForNet(PipId pip, const NetInfo *net) const override
    {
        uint32_t flags = chip_pip_info(ctx->chip_info, pip).flags;
        if ((flags & FLAG_GENERAL) && restricted.count(net->name)) {
            // (but the sinks' own wires)
            WireId dst = ctx->getPipDstWire(pip);
            bool sink = false;
            for (auto &usr : net->users)
                for (WireId w : ctx->getNetinfoSinkWires(net, usr))
                    sink |= w == dst;
            if (!sink)
                return false;
        }
        if (!(flags & (FLAG_RT_FREE | FLAG_RT_OWN)))
            return true;
        auto [x, y, s] = rt_site(pip);
        BelId bel = ctx->getBelByLocation(Loc(x, y, s));
        CellInfo *on_site = bel == BelId() ? nullptr : ctx->getBoundBelCell(bel);
        if (flags & FLAG_RT_OWN)
            return on_site && on_site == net->driver.cell && net->driver.port == id_COUT && !on_site->getPort(id_DMUX);
        if (on_site)
            return false;
        auto it = rt_use.find(std::make_tuple(x, y, s));
        return it == rt_use.end() || it->second.first == net || it->second.second == 0;
    }

    // (router2 asks for each net: router1 asks only checkPipAvail, and would ignore the rules above)
    std::string getDefaultRouter() const override { return "router2"; }

    void notifyPipChange(PipId pip, NetInfo *net) override
    {
        if (!(chip_pip_info(ctx->chip_info, pip).flags & FLAG_RT_FREE))
            return;
        auto &use = rt_use[rt_site(pip)];
        if (net) {
            use.first = net;
            use.second++;
        } else if (--use.second == 0)
            use.first = nullptr;
    }

    // ------------------------------------------------------------------ timing: each cell's own arcs (TMG)
    bool getCellDelay(const CellInfo *cell, IdString fromPort, IdString toPort, DelayQuad &delay) const override
    {
        auto t = timing.find(cell->name);
        if (t == timing.end())
            return false;
        auto it = t->second.comb.find(std::make_pair(fromPort, toPort));
        if (it == t->second.comb.end())
            return false;
        delay = DelayQuad(it->second);
        return true;
    }

    TimingPortClass getPortTimingClass(const CellInfo *cell, IdString port, int &clockInfoCount) const override
    {
        clockInfoCount = 0;
        auto it = timing.find(cell->name);
        if (it == timing.end())
            return TMG_IGNORE;
        const CellTiming &t = it->second;
        if (t.clocks.count(port))
            return TMG_CLOCK_INPUT;
        if (t.setup.count(port)) {
            clockInfoCount = 1;
            return TMG_REGISTER_INPUT;
        }
        if (t.clock_out.count(port)) {
            clockInfoCount = 1;
            return TMG_REGISTER_OUTPUT;
        }
        if (t.comb_in.count(port))
            return TMG_COMB_INPUT;
        if (t.comb_out.count(port))
            return TMG_COMB_OUTPUT;
        return TMG_IGNORE;
    }

    TimingClockingInfo getPortClockingInfo(const CellInfo *cell, IdString port, int index) const override
    {
        const CellTiming &t = timing.at(cell->name);
        TimingClockingInfo info;
        auto s = t.setup.find(port);
        if (s != t.setup.end()) {
            info.clock_port = s->second.first;
            info.setup = DelayPair(s->second.second);
            info.hold = DelayPair(0);
            info.clockToQ = DelayQuad(0);
        } else {
            auto &q = t.clock_out.at(port);
            info.clock_port = q.first;
            info.setup = DelayPair(0);
            info.hold = DelayPair(0);
            info.clockToQ = DelayQuad(q.second);
        }
        auto k = t.clocks.find(info.clock_port);
        info.edge = k != t.clocks.end() && !k->second ? FALLING_EDGE : RISING_EDGE;
        return info;
    }

    // An unrouted connection's delay from the Manhattan distance in tiles (Silinx's fpga/s6/place-timing.js S6_ESTIMATE)
    static delay_t distance_delay(int d) { return 450 + 60 * d; }

    delay_t estimateDelay(WireId src, WireId dst) const override
    {
        int sx, sy, dx, dy;
        tile_xy(ctx->chip_info, src.tile, sx, sy);
        tile_xy(ctx->chip_info, dst.tile, dx, dy);
        return distance_delay(std::abs(dx - sx) + std::abs(dy - sy));
    }

    delay_t predictDelay(BelId src_bel, IdString src_pin, BelId dst_bel, IdString dst_pin) const override
    {
        Loc s = ctx->getBelLocation(src_bel), d = ctx->getBelLocation(dst_bel);
        return distance_delay(std::abs(d.x - s.x) + std::abs(d.y - s.y));
    }
};

struct Spartan6Arch : HimbaechelArch
{
    Spartan6Arch() : HimbaechelArch("spartan6") {};
    bool match_device(const std::string &device) override { return device.rfind("xc6s", 0) == 0; }
    std::unique_ptr<HimbaechelAPI> create(const std::string &device) override
    {
        return std::make_unique<Spartan6Impl>(device);
    }
} spartan6Arch;

} // namespace

NEXTPNR_NAMESPACE_END
