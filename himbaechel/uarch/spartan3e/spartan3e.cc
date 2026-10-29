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

// Spartan-3E, a first micro-architecture (a spike): LUT4s, flip-flops, IOBs and BUFGMUXes, from the
// netlist of Yosys's synth_xilinx -family xc3se -nocarry -nowidelut. The chip database is built from
// Silinx's device graph (gen/s3e_gen.py).

#include <fstream>
#include <regex>

#include "log.h"
#include "nextpnr.h"
#include "util.h"

#include "himbaechel_helpers.h"

#define GEN_INIT_CONSTIDS
#define HIMBAECHEL_CONSTIDS "uarch/spartan3e/constids.inc"
#define HIMBAECHEL_UARCH spartan3e
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

// BELs of a site: z = site index * Z_STRIDE + offset (gen/s3e_gen.py)
constexpr int Z_STRIDE = 8;
constexpr int Z_FLUT = 0, Z_FFX = 2, Z_FFY = 3;

struct Spartan3EImpl : HimbaechelAPI
{
    std::string device;
    HimbaechelHelpers h;

    Spartan3EImpl(const std::string &device) : device(device) {}
    ~Spartan3EImpl() {};

    po::options_description getUArchOptions() override
    {
        po::options_description specific("Spartan-3E specific options");
        return specific;
    }

    void init_database(Arch *arch) override
    {
        init_uarch_constids(arch);
        // xc3s250ecp132-4: the part's database, its package, its speed grade
        std::smatch m;
        if (!std::regex_match(device, m, std::regex("(xc3s[0-9]+e)([a-z]+[0-9]+)(?:-([0-9]))?")))
            log_error("Unknown Spartan-3E device '%s' (e.g. xc3s250ecp132-4).\n", device.c_str());
        arch->load_chipdb(stringf("spartan3e/chipdb-%s.bin", m[1].str().c_str()));
        std::string pkg = m[2].str();
        for (auto &c : pkg)
            c = toupper(c);
        arch->set_package(pkg);
        arch->set_speed_grade("4");
    }

    void init(Context *ctx) override
    {
        h.init(ctx);
        HimbaechelAPI::init(ctx);
    }

    // ------------------------------------------------------------------ packing
    void rename_port(CellInfo *ci, const char *from, IdString to)
    {
        IdString f = ctx->id(from);
        if (ci->ports.count(f))
            ci->renamePort(f, to);
    }

    // a port tied to a constant: true if tied to `value` ('0' / '1'), and then disconnected
    bool drop_constant(CellInfo *ci, IdString port, IdString value)
    {
        NetInfo *n = ci->getPort(port);
        if (!n || !n->driver.cell || n->driver.cell->type != value)
            return false;
        ci->disconnectPort(port);
        return true;
    }

    void pack_ios()
    {
        // a top port's LOC (its attribute in the netlist) goes to the IBUF / OBUF on it
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (!ci->type.in(ctx->id("$nextpnr_ibuf"), ctx->id("$nextpnr_obuf")) || !ci->attrs.count(id_LOC))
                continue;
            NetInfo *n = ci->getPort(ci->type == ctx->id("$nextpnr_ibuf") ? id_O : id_I);
            if (n && n->driver.cell && n->driver.cell->type == id_OBUF)
                n->driver.cell->attrs[id_LOC] = ci->attrs.at(id_LOC);
            if (n)
                for (auto &usr : n->users)
                    if (usr.cell->type == id_IBUF)
                        usr.cell->attrs[id_LOC] = ci->attrs.at(id_LOC);
        }
        h.remove_nextpnr_iobs({CellTypePort(id_IBUF, id_I), CellTypePort(id_OBUF, id_O)});
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_IBUF) {
                ci->renamePort(id_I, id_PAD);
                ci->renamePort(id_O, id_I);
            } else if (ci->type == id_OBUF) {
                ci->renamePort(id_O, id_PAD);
                ci->renamePort(id_I, id_O);
            } else
                continue;
            ci->type = id_IOB;
            // its pad, from its LOC
            if (ci->attrs.count(id_LOC)) {
                std::string pin = ci->attrs.at(id_LOC).as_string();
                BelId bel = ctx->get_package_pin_bel(ctx->id(pin));
                if (bel == BelId())
                    log_error("IO '%s': no pad '%s' in this package.\n", ctx->nameOf(ci), pin.c_str());
                ci->attrs[id_BEL] = std::string(ctx->nameOfBel(bel));
            }
        }
    }

    void pack_luts_ffs()
    {
        static const std::regex lut_re("LUT([1-4])");
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            std::string type = ci->type.str(ctx);
            std::smatch m;
            if (ci->type == id_INV) {               // a LUT1 with INIT 01
                ci->renamePort(id_I, id_I0);
                ci->params[id_INIT] = Property(0x5555, 16);
                ci->type = id_LUT4;
            } else if (std::regex_match(type, m, lut_re)) {
                int k = std::stoi(m[1]);
                // the INIT of k inputs repeated over the 16 entries of the LUT4 (the unused inputs don't matter)
                Property init = get_or_default(ci->params, id_INIT, Property(0, 1 << k));
                uint64_t v = init.as_int64() & ((1ULL << (1 << k)) - 1), w = 0;
                for (int i = 0; i < 16; i++)
                    w |= ((v >> (i % (1 << k))) & 1ULL) << i;
                ci->params[id_INIT] = Property(w, 16);
                ci->type = id_LUT4;
            } else if (type.size() >= 2 && type.substr(0, 2) == "FD") {
                // FD[R|S|C|P][E]: synchronous reset / set, asynchronous clear / preset, clock enable
                std::string sr = "NONE";
                const char *srp = nullptr;
                if (type.find('R') != std::string::npos)
                    sr = "SYNC_RESET", srp = "R";
                else if (type.find('S') != std::string::npos)
                    sr = "SYNC_SET", srp = "S";
                else if (type.find('C') != std::string::npos)
                    sr = "ASYNC_RESET", srp = "CLR";
                else if (type.find('P') != std::string::npos)
                    sr = "ASYNC_SET", srp = "PRE";
                rename_port(ci, "C", id_CLK);
                if (srp)
                    rename_port(ci, srp, id_SR);
                ci->params[id_SRMODE] = std::string(sr);
                if (drop_constant(ci, id_CE, id_VCC) || !ci->getPort(id_CE))
                    ci->disconnectPort(id_CE);
                if (drop_constant(ci, id_SR, id_GND))
                    ci->params[id_SRMODE] = std::string("NONE");
                ci->type = id_FF;
            } else if (ci->type == id_BUFG) {
                ci->renamePort(id_I, id_I0);
                ci->type = id_BUFGMUX;
            }
        }
    }

    void pack() override
    {
        pack_ios();
        pack_luts_ffs();
        // the remaining users of '0' and '1' are not handled by the spike
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type != id_GND && ci->type != id_VCC)
                continue;
            for (auto &port : ci->ports)
                if (port.second.net && port.second.net->users.entries() > 0)
                    for (auto &usr : port.second.net->users)
                        log_error("'%s' of '%s' is tied to a constant: not supported yet.\n", usr.port.c_str(ctx),
                                  ctx->nameOf(usr.cell));
        }
        std::vector<IdString> trim;
        for (auto &cell : ctx->cells)
            if (cell.second->type == id_GND || cell.second->type == id_VCC)
                trim.push_back(cell.first);
        for (auto c : trim) {
            CellInfo *ci = ctx->cells.at(c).get();
            for (auto &port : ci->ports)
                if (port.second.net)
                    ctx->nets.erase(port.second.net->name);
            ctx->cells.erase(c);
        }
        pack_slices();
    }

    // ------------------------------------------------------------------ slices
    // A unit: a LUT and the flip-flop it feeds, or a LUT or a flip-flop alone; a slice takes two (its F
    // side: F, FFX; its G side: G, FFY), whose flip-flops share CLK, CE, SR and the set / reset mode.
    struct Unit
    {
        CellInfo *lut = nullptr, *ff = nullptr;
        int mate = -1;
    };

    static bool same_control(const CellInfo *a, const CellInfo *b)
    {
        if (!a || !b)
            return true;
        for (IdString p : {id_CLK, id_CE, id_SR})
            if (a->getPort(p) != b->getPort(p))
                return false;
        return a->params.at(id_SRMODE) == b->params.at(id_SRMODE);
    }

    void pack_slices()
    {
        std::vector<Unit> units;
        dict<IdString, int> unit_of;
        // a flip-flop with its LUT: the LUT driving its D, not yet in a unit
        for (auto &cell : ctx->cells) {
            CellInfo *ff = cell.second.get();
            if (ff->type != id_FF)
                continue;
            NetInfo *d = ff->getPort(id_D);
            CellInfo *lut = d ? d->driver.cell : nullptr;
            if (lut && (lut->type != id_LUT4 || unit_of.count(lut->name)))
                lut = nullptr;
            unit_of[ff->name] = units.size();
            if (lut)
                unit_of[lut->name] = units.size();
            units.push_back(Unit{lut, ff});
        }
        for (auto &cell : ctx->cells)
            if (cell.second->type == id_LUT4 && !unit_of.count(cell.first)) {
                unit_of[cell.first] = units.size();
                units.push_back(Unit{cell.second.get(), nullptr});
            }
        // two units to a slice: the compatible one sharing the most nets (each net weighs 1 / its fanout,
        // the clocks and the resets of many flip-flops next to nothing), then the rest by control set
        auto nets_of = [&](const Unit &u) {
            std::vector<NetInfo *> nets;
            for (CellInfo *c : {u.lut, u.ff})
                if (c)
                    for (auto &port : c->ports)
                        if (port.second.net)
                            nets.push_back(port.second.net);
            return nets;
        };
        for (int i = 0; i < int(units.size()); i++) {
            if (units[i].mate >= 0)
                continue;
            dict<int, float> score;
            for (NetInfo *n : nets_of(units[i])) {
                int fanout = n->users.entries() + 1;
                if (fanout > 64)
                    continue;
                auto add = [&](CellInfo *c) {
                    auto it = unit_of.find(c->name);
                    if (it != unit_of.end() && it->second != i)
                        score[it->second] += 1.0f / fanout;
                };
                if (n->driver.cell)
                    add(n->driver.cell);
                for (auto &usr : n->users)
                    add(usr.cell);
            }
            int best = -1;
            float best_score = 0;
            for (auto &s : score)
                if (units[s.first].mate < 0 && same_control(units[i].ff, units[s.first].ff) &&
                    (s.second > best_score || (s.second == best_score && s.first < best)))
                    best = s.first, best_score = s.second;
            if (best >= 0)
                units[i].mate = best, units[best].mate = i;
        }
        for (int i = 0; i < int(units.size()); i++)
            for (int j = i + 1; j < int(units.size()) && units[i].mate < 0; j++)
                if (units[j].mate < 0 && same_control(units[i].ff, units[j].ff))
                    units[i].mate = j, units[j].mate = i;
        // the clusters: F 0, G 1, FFX 2, FFY 3 within the slice, from the cell of the lowest
        int slices = 0;
        for (int i = 0; i < int(units.size()); i++) {
            int j = units[i].mate;
            if (j >= 0 && j < i)
                continue;
            std::vector<std::pair<CellInfo *, int>> cells;
            for (auto [u, side] : {std::make_pair(i, 0), std::make_pair(j, 1)}) {
                if (u < 0)
                    continue;
                if (units[u].lut)
                    cells.emplace_back(units[u].lut, Z_FLUT + side);
                if (units[u].ff)
                    cells.emplace_back(units[u].ff, Z_FFX + side);
            }
            std::sort(cells.begin(), cells.end(), [](auto &a, auto &b) { return a.second < b.second; });
            CellInfo *root = cells.front().first;
            root->cluster = root->name;
            root->constr_abs_z = false;
            for (size_t k = 1; k < cells.size(); k++) {
                CellInfo *c = cells[k].first;
                root->constr_children.push_back(c);
                c->cluster = root->name;
                c->constr_x = 0;
                c->constr_y = 0;
                c->constr_z = cells[k].second - cells.front().second;
                c->constr_abs_z = false;
            }
            slices++;
        }
        log_info("Packed %d LUTs and flip-flops into %d slices.\n", int(unit_of.size()), slices);
    }

    // ------------------------------------------------------------------ delay estimates
    // An unrouted connection's delay from the Manhattan distance d in tiles, Silinx's (fpga/timing.js
    // SPEED_4.dist, fitted to ISE's trce): base + perTile x min(d, knee) + far x (d - knee) beyond the knee (long lines).
    static delay_t distance_delay(int d)
    {
        constexpr int base = 550, per_tile = 100, knee = 12, far = 30;
        return base + per_tile * std::min(d, knee) + far * std::max(0, d - knee);
    }

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
        // a LUT to its own flip-flop: inside the slice
        if (s.x == d.x && s.y == d.y && s.z / Z_STRIDE == d.z / Z_STRIDE && dst_pin == id_D)
            return 0;
        return distance_delay(std::abs(d.x - s.x) + std::abs(d.y - s.y));
    }

    // ------------------------------------------------------------------ validity
    // The two flip-flops of a slice share CLK, CE, SR and the set / reset mode.
    bool isBelLocationValid(BelId bel, bool explain_invalid) const override
    {
        Loc l = ctx->getBelLocation(bel);
        int off = l.z % Z_STRIDE;
        if (off != Z_FFX && off != Z_FFY)
            return true;
        BelId other = ctx->getBelByLocation(Loc(l.x, l.y, l.z - off + (off == Z_FFX ? Z_FFY : Z_FFX)));
        const CellInfo *a = ctx->getBoundBelCell(bel), *b = other == BelId() ? nullptr : ctx->getBoundBelCell(other);
        if (!a || !b)
            return true;
        for (IdString p : {id_CLK, id_CE, id_SR})
            if (a->getPort(p) != b->getPort(p)) {
                if (explain_invalid)
                    log_nonfatal_error("%s and %s: different %s in one slice\n", ctx->nameOf(a), ctx->nameOf(b),
                                       p.c_str(ctx));
                return false;
            }
        return a->params.at(id_SRMODE) == b->params.at(id_SRMODE);
    }
};

struct Spartan3EArch : HimbaechelArch
{
    Spartan3EArch() : HimbaechelArch("spartan3e") {};
    bool match_device(const std::string &device) override { return device.rfind("xc3s", 0) == 0; }
    std::unique_ptr<HimbaechelAPI> create(const std::string &device) override
    {
        return std::make_unique<Spartan3EImpl>(device);
    }
} spartan3eArch;

} // namespace

NEXTPNR_NAMESPACE_END
