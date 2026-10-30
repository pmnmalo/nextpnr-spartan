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

// Spartan-3E: LUT4s, flip-flops, carry chains, F5 to F8 multiplexers, IOBs with their registers and BUFGMUXes, from the netlist of
// Yosys's synth_xilinx -family xc3se (Silinx's synthesis). The chip database is built from
// Silinx's device graph (gen/s3e_gen.py).

#include <algorithm>
#include <fstream>
#include <set>
#include <functional>
#include <map>
#include <queue>
#include <regex>
#include <unordered_map>

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
constexpr int Z_STRIDE = 16;
constexpr int Z_FLUT = 0, Z_GLUT = 1, Z_FFX = 2, Z_FFY = 3, Z_CYMUXF = 4, Z_CYMUXG = 5, Z_XORF = 6, Z_XORG = 7;
constexpr int Z_F5MUX = 8, Z_FXMUX = 9, Z_FMEM = 10, Z_GMEM = 11, Z_GND = 15;
constexpr int Z_IFF1 = 1, Z_IFF2 = 2, Z_OFF1 = 3, Z_OFF2 = 4, Z_TFF1 = 5;

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
            if (n && n->driver.cell && n->driver.cell->type.in(id_OBUF, id_OBUFT))
                n->driver.cell->attrs[id_LOC] = ci->attrs.at(id_LOC);
            if (n)
                for (auto &usr : n->users)
                    if (usr.cell->type.in(id_IBUF, id_IBUFG))
                        usr.cell->attrs[id_LOC] = ci->attrs.at(id_LOC);
        }
        h.remove_nextpnr_iobs({CellTypePort(id_IBUF, id_I), CellTypePort(id_IBUFG, id_I), CellTypePort(id_OBUF, id_O),
                               CellTypePort(id_OBUFT, id_O), CellTypePort(id_IOBUF, id_IO)});
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type.in(id_IBUF, id_IBUFG)) {
                ci->renamePort(id_I, id_PAD);
                ci->renamePort(id_O, id_I);
            } else if (ci->type.in(id_OBUF, id_OBUFT)) {
                ci->renamePort(id_O, id_PAD);
                ci->renamePort(id_I, id_O);
            } else if (ci->type == id_IOBUF) {   // (its I and O: the fabric's; the IOB's I comes from the pad)
                ci->renamePort(id_IO, id_PAD);
                ci->renamePort(id_O, id_D);
                ci->renamePort(id_I, id_O);
                ci->renamePort(id_D, id_I);
            } else
                continue;
            // an output-enable tied to 0: always driving, T1 not used
            drop_constant(ci, id_T, id_GND);
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
        pack_iob_ddr();
        pack_iob_regs();
        pack_clock_pads();
        pack_dcms();
        pack_muxes();
        pack_hard();
        pack_mems();
        fold_constants();
        pack_carry();
        constants_to_luts();
        pack_slices();
    }

    // ------------------------------------------------------------------ registers in the IOBs
    // As Silinx's packer (fpga/pack-iobregs.js), so as ISE's map: a flip-flop goes into its pad's IOB when asked (the
    // attribute IOB on it or on the pad's buffer: Silinx's nextpnrNetlist writes the UCF's IOB = TRUE / FALSE there), its
    // clock on a global buffer; an input register's D is the pad's input, an output (output-enable) register's Q drives
    // only O (T), the two sharing the clock and the set / reset. An IDDR2 / ODDR2 is always in its IOB, as two flip-flops.
    static bool asked_true(const Property &p)
    {
        std::string v = p.is_string ? p.as_string() : std::to_string(p.as_int64());
        for (auto &c : v)
            c = toupper(c);
        return v == "TRUE" || v == "FORCE" || v == "1";
    }

    static bool on_global(const NetInfo *n) { return n && n->driver.cell && n->driver.cell->type == id_BUFGMUX; }

    bool wants_iob(CellInfo *ff, CellInfo *iob)
    {
        auto a = ff->attrs.find(id_IOB);
        bool want = false;
        if (a != ff->attrs.end())
            want = asked_true(a->second);
        else if (iob->attrs.count(id_IOB))
            want = asked_true(iob->attrs.at(id_IOB));
        if (want && !on_global(ff->getPort(id_CLK))) {
            log_warning("flip-flop %s kept out of the IOB of %s: its clock is not on a global clock buffer\n", ctx->nameOf(ff), ctx->nameOf(iob));
            return false;
        }
        return want;
    }

    // a register at its place in its IOB's site (the IOB placed by its LOC)
    void place_in_iob(CellInfo *ff, CellInfo *iob, IdString type, int dz)
    {
        if (!iob->attrs.count(id_BEL))
            log_error("%s: a register in the IOB of '%s', which has no LOC.\n", ctx->nameOf(ff), ctx->nameOf(iob));
        Loc l = ctx->getBelLocation(ctx->getBelByNameStr(iob->attrs.at(id_BEL).as_string()));
        ff->type = type;
        ff->attrs[id_BEL] = std::string(ctx->nameOfBel(ctx->getBelByLocation(Loc(l.x, l.y, l.z + dz))));
        // (a flip-flop on the inverse of its clock net: the falling edge's timing)
        bool falling = ff->params.count(id_CLKINV) && ff->params.at(id_CLKINV).as_int64();
        if (falling)
            ff->timing_index = ctx->get_cell_timing_idx(ctx->idf("%sN", type.c_str(ctx)));
    }

    CellInfo *sole_ff(NetInfo *n)
    {
        CellInfo *d = n ? n->driver.cell : nullptr;
        return d && d->type == id_FF && n->users.entries() == 1 ? d : nullptr;
    }

    void pack_iob_regs()
    {
        int n = 0;
        std::vector<CellInfo *> iobs;
        for (auto &cell : ctx->cells)
            if (cell.second->type == id_IOB)
                iobs.push_back(cell.second.get());
        for (CellInfo *iob : iobs) {
            if (NetInfo *i = iob->getPort(id_I)) {
                for (auto &usr : i->users)
                    if (usr.cell->type == id_FF && usr.port == id_D && wants_iob(usr.cell, iob)) {
                        place_in_iob(usr.cell, iob, id_IFF, Z_IFF1);
                        n++;
                        break;
                    }
            }
            CellInfo *o = sole_ff(iob->getPort(id_O)), *t = sole_ff(iob->getPort(id_T));
            if (o && !wants_iob(o, iob))
                o = nullptr;
            if (t && !wants_iob(t, iob))
                t = nullptr;
            // (one clock and one set / reset for the two)
            if (o && t && (o->getPort(id_CLK) != t->getPort(id_CLK) || o->getPort(id_SR) != t->getPort(id_SR) ||
                           (o->params.at(id_SRMODE).as_string().find("ASYNC") == std::string::npos) !=
                                   (t->params.at(id_SRMODE).as_string().find("ASYNC") == std::string::npos)))
                t = nullptr;
            if (o) {
                place_in_iob(o, iob, id_OFF, Z_OFF1);
                n++;
            }
            if (t) {
                place_in_iob(t, iob, id_TFF, Z_TFF1);
                n++;
            }
        }
        if (n)
            log_info("Packed %d flip-flop(s) into IOBs.\n", n);
    }

    // a clock of a DDR register: an inverter on a global buffer's net is the net itself, inverted (ICLK2INV = ICLK2_B)
    void ddr_clock(CellInfo *ddr, IdString port, CellInfo *ff0, const char *inverted, std::vector<IdString> &dead)
    {
        NetInfo *n = ddr->getPort(port);
        bool inv = ddr->params.count(ctx->id(inverted)) && ddr->params.at(ctx->id(inverted)).as_int64();
        CellInfo *d = n ? n->driver.cell : nullptr;
        if (d && d->type == id_LUT4 && d->params.at(id_INIT).as_int64() == 0x5555 && on_global(d->getPort(id_I0))) {
            NetInfo *in = d->getPort(id_I0);
            ddr->disconnectPort(port);
            if (n->users.entries() == 0)
                dead.push_back(d->name);
            n = in;
            inv = !inv;
        }
        if (!on_global(n))
            log_error("%s %s: its clock %s is not on a global clock buffer (BUFG).\n", ddr->type.c_str(ctx), ctx->nameOf(ddr), port.c_str(ctx));
        ff0->connectPort(id_CLK, n);
        ff0->params[id_CLKINV] = Property(inv ? 1 : 0, 1);
    }

    // a constant's net, for a pin ISE's map ties to it (an unconnected enable or set / reset)
    NetInfo *constant_net(IdString name, IdString type)
    {
        CellInfo *c = ctx->createCell(name, type);
        c->addOutput(id_O);
        NetInfo *n = ctx->createNet(ctx->idf("%s$o", name.c_str(ctx)));
        c->connectPort(id_O, n);
        return n;
    }

    void pack_iob_ddr()
    {
        std::vector<CellInfo *> ddrs;
        for (auto &cell : ctx->cells)
            if (cell.second->type.in(id_IDDR2, id_ODDR2))
                ddrs.push_back(cell.second.get());
        std::vector<IdString> dead;
        for (CellInfo *c : ddrs) {
            bool in = c->type == id_IDDR2;
            std::string align = c->params.count(ctx->id("DDR_ALIGNMENT")) ? c->params.at(ctx->id("DDR_ALIGNMENT")).as_string() : "NONE";
            if (align != "NONE")
                log_error("%s %s: DDR_ALIGNMENT = %s is not supported (only NONE).\n", c->type.c_str(ctx), ctx->nameOf(c), align.c_str());
            NetInfo *pad_net = c->getPort(in ? id_D : ctx->id("Q"));
            CellInfo *iob = nullptr;
            if (in && pad_net && pad_net->driver.cell && pad_net->driver.cell->type == id_IOB)
                iob = pad_net->driver.cell;
            if (!in && pad_net && pad_net->users.entries() == 1 && (*pad_net->users.begin()).cell->type == id_IOB && (*pad_net->users.begin()).port == id_O)
                iob = (*pad_net->users.begin()).cell;
            if (!iob)
                log_error("%s %s: %s (the DDR registers are in the IOBs).\n", c->type.c_str(ctx), ctx->nameOf(c),
                          in ? "its D must come straight from an input port" : "its Q must drive an output port and nothing else");
            auto used = [&](IdString p) { NetInfo *n = c->getPort(p); return n && !is_constant(n, id_GND); };
            if (!in && used(id_R) && used(id_S))
                log_error("ODDR2 %s: both R and S used (the IOB has one set / reset for its output registers).\n", ctx->nameOf(c));
            std::string srtype = c->params.count(ctx->id("SRTYPE")) ? c->params.at(ctx->id("SRTYPE")).as_string() : "SYNC";
            for (auto &ch : srtype)
                ch = toupper(ch);
            NetInfo *ce = c->getPort(id_CE), *r = c->getPort(id_R), *s = c->getPort(id_S), *d0 = c->getPort(in ? id_D : id_D0), *d1 = c->getPort(in ? id_D : id_D1);
            NetInfo *q0 = c->getPort(ctx->id("Q0")), *q1 = c->getPort(ctx->id("Q1"));
            // the enable and the set / reset: their nets or the constants ISE's map ties them to
            std::string base = c->name.str(ctx);
            if (!ce)
                ce = constant_net(ctx->id(base + "$ce"), id_VCC);
            NetInfo *sr = !in && used(id_S) ? s : r;
            if (!sr)
                sr = constant_net(ctx->id(base + "$sr"), id_GND);
            NetInfo *rev = in ? s : nullptr;
            if (in && !rev)
                rev = constant_net(ctx->id(base + "$rev"), id_GND);
            std::string mode = srtype + (!in && used(id_S) ? "_SET" : "_RESET");
            for (int k = 0; k < 2; k++) {
                CellInfo *f = ctx->createCell(ctx->idf("%s/FF%d", base.c_str(), k), id_FF);
                for (IdString p : {id_D, id_CLK, id_CE, id_SR})
                    f->addInput(p);
                f->addOutput(id_Q);
                f->connectPort(id_D, k ? d1 : d0);
                f->connectPort(id_CE, ce);
                f->connectPort(id_SR, sr);
                if (in) {
                    f->addInput(id_REV);
                    f->connectPort(id_REV, rev);
                }
                const char *init = in ? (k ? "INIT_Q1" : "INIT_Q0") : "INIT";
                f->params[id_INIT] = c->params.count(ctx->id(init)) ? c->params.at(ctx->id(init)) : Property(0, 1);
                f->params[id_SRMODE] = mode;
                ddr_clock(c, k ? id_C1 : id_C0, f, k ? "IS_C1_INVERTED" : "IS_C0_INVERTED", dead);
                NetInfo *q = in ? (k ? q1 : q0) : (k ? nullptr : pad_net);
                c->disconnectPort(in ? ctx->idf("Q%d", k) : ctx->id("Q"));
                if (q)
                    f->connectPort(id_Q, q);
                place_in_iob(f, iob, in ? id_IFF : id_OFF, in ? (k ? Z_IFF2 : Z_IFF1) : (k ? Z_OFF2 : Z_OFF1));
            }
            for (auto &port : c->ports)
                if (port.second.net)
                    c->disconnectPort(port.first);
            dead.push_back(c->name);
        }
        for (IdString name : dead) {
            CellInfo *d = ctx->cells.at(name).get();
            NetInfo *o = d->getPort(id_O);
            for (auto &port : d->ports)
                if (port.second.net)
                    d->disconnectPort(port.first);
            if (o)
                ctx->nets.erase(o->name);
            ctx->cells.erase(name);
        }
    }

    // ------------------------------------------------------------------ clock pads
    // A global buffer driven by a pad goes where the pad's dedicated path reaches it (the pad's I, the clock tile's own
    // wires, none of the general routing: Silinx's bufgReach in fpga/route.js), the router then taking that path.
    static bool general_wire(const std::string &w)
    {
        static const std::regex re("^([NSEW](2|6)(BEG|MID|END|[A-D])[0-9]|L[HV][0-9]).*");
        return std::regex_match(w, re);
    }

    // the BELs of `type` whose `pin` a wire reaches by dedicated paths, the nearest first
    std::vector<BelId> dedicated_reach(WireId start, IdString type, IdString pin) const
    {
        std::vector<BelId> found;
        std::vector<WireId> queue{start};
        pool<WireId> seen{start};
        for (size_t i = 0; i < queue.size() && i < 20000; i++) {
            for (auto bp : ctx->getWireBelPins(queue[i]))
                if (bp.pin == pin && ctx->getBelType(bp.bel) == type && std::find(found.begin(), found.end(), bp.bel) == found.end())
                    found.push_back(bp.bel);
            for (PipId p : ctx->getPipsDownhill(queue[i])) {
                WireId d = ctx->getPipDstWire(p);
                if (!seen.count(d) && !general_wire(ctx->getPipName(p)[1].str(ctx))) {
                    seen.insert(d);
                    queue.push_back(d);
                }
            }
        }
        return found;
    }

    pool<BelId> taken_buffers;

    BelId dedicated_buffer(BelId pad) const
    {
        for (BelId b : dedicated_reach(ctx->getBelPinWire(pad, id_I), id_BUFGMUX, id_I0))
            if (!taken_buffers.count(b))
                return b;
        return BelId();
    }

    void pack_clock_pads()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            NetInfo *n = ci->type == id_BUFGMUX ? ci->getPort(id_I0) : nullptr;
            CellInfo *d = n ? n->driver.cell : nullptr;
            if (!d || d->type != id_IOB || !d->attrs.count(id_BEL) || ci->attrs.count(id_BEL))
                continue;
            BelId b = dedicated_buffer(ctx->getBelByNameStr(d->attrs.at(id_BEL).as_string()));
            if (b == BelId())
                continue;
            taken_buffers.insert(b);
            ci->attrs[id_BEL] = std::string(ctx->nameOfBel(b));
            log_info("Clock pad %s: on its dedicated path to %s.\n", ctx->nameOf(d), ctx->nameOfBel(b));
        }
    }

    // ------------------------------------------------------------------ DCMs
    // A DCM / DCM_SP on the site where the most of its clock outputs reach global buffers of their own by dedicated paths
    // (Silinx's place-dcms.js), then where its CLKIN comes from a pad's dedicated path; its site given by the UCF's INST
    // LOC (the attribute BEL, from Silinx's nextpnrNetlist) is kept. The buffers go there too. Placed here, not by the
    // placer: the buffers' places depend on it. A constant on DSSEN is left open, as ISE's map does.
    void pack_dcms()
    {
        static const std::regex status_re("STATUS\\[(\\d+)\\]");
        static const std::set<std::string> pins = {"CLKIN", "CLKFB", "RST", "DSSEN", "PSCLK", "PSEN", "PSINCDEC", "CLK0", "CLK90", "CLK180", "CLK270",
                                                   "CLK2X", "CLK2X180", "CLKDV", "CLKFX", "CLKFX180", "LOCKED", "PSDONE"};
        std::vector<CellInfo *> dcms;
        for (auto &cell : ctx->cells)
            if (cell.second->type.in(id_DCM, id_DCM_SP))
                dcms.push_back(cell.second.get());
        pool<BelId> used;
        for (CellInfo *c : dcms)
            if (c->attrs.count(id_BEL))
                used.insert(ctx->getBelByNameStr(c->attrs.at(id_BEL).as_string()));
        for (CellInfo *c : dcms) {
            // the buffers on its clock outputs
            std::vector<std::pair<IdString, CellInfo *>> buffers;
            for (auto &port : c->ports)
                if (port.second.type == PORT_OUT && port.second.net)
                    for (auto &usr : port.second.net->users)
                        if (usr.cell->type == id_BUFGMUX && usr.port == id_I0 && !usr.cell->attrs.count(id_BEL))
                            buffers.emplace_back(port.first, usr.cell);
            NetInfo *in = c->getPort(id_CLKIN);
            CellInfo *pad = in && in->driver.cell && in->driver.cell->type == id_IOB && in->driver.cell->attrs.count(id_BEL) ? in->driver.cell : nullptr;
            std::vector<BelId> from_pad;
            if (pad)
                from_pad = dedicated_reach(ctx->getBelPinWire(ctx->getBelByNameStr(pad->attrs.at(id_BEL).as_string()), id_I), id_DCM, id_CLKIN);
            std::vector<BelId> sites;
            if (c->attrs.count(id_BEL))
                sites.push_back(ctx->getBelByNameStr(c->attrs.at(id_BEL).as_string()));
            else
                for (BelId b : ctx->getBels())
                    if (ctx->getBelType(b) == id_DCM && !used.count(b))
                        sites.push_back(b);
            if (sites.empty())
                log_error("DCM %s: no free DCM site.\n", ctx->nameOf(c));
            BelId best;
            int best_score = -1;
            std::vector<std::pair<CellInfo *, BelId>> best_match;
            for (BelId site : sites) {
                // (the outputs with the fewest buffers first)
                std::vector<std::pair<CellInfo *, std::vector<BelId>>> options;
                for (auto &b : buffers)
                    options.emplace_back(b.second, dedicated_reach(ctx->getBelPinWire(site, b.first), id_BUFGMUX, id_I0));
                std::stable_sort(options.begin(), options.end(), [](auto &x, auto &y) { return x.second.size() < y.second.size(); });
                pool<BelId> mine;
                std::vector<std::pair<CellInfo *, BelId>> match;
                for (auto &o : options)
                    for (BelId b : o.second)
                        if (!taken_buffers.count(b) && !mine.count(b)) {
                            mine.insert(b);
                            match.emplace_back(o.first, b);
                            break;
                        }
                int score = int(match.size()) * 2 + (std::find(from_pad.begin(), from_pad.end(), site) != from_pad.end() ? 1 : 0);
                if (score > best_score)
                    best = site, best_score = score, best_match = match;
            }
            used.insert(best);
            c->attrs[id_BEL] = std::string(ctx->nameOfBel(best));
            for (auto &m : best_match) {
                m.first->attrs[id_BEL] = std::string(ctx->nameOfBel(m.second));
                taken_buffers.insert(m.second);
            }
            log_info("DCM %s on %s: %d of %d clock output(s) on their dedicated paths to global buffers.\n", ctx->nameOf(c), ctx->nameOfBel(best),
                     int(best_match.size()), int(buffers.size()));
            NetInfo *dssen = c->getPort(ctx->id("DSSEN"));
            if (dssen && dssen->driver.cell && dssen->driver.cell->type.in(id_GND, id_VCC))
                c->disconnectPort(ctx->id("DSSEN"));
            c->type = id_DCM;
            hard_ports(c, [&](const std::string &name) -> std::vector<std::string> {
                std::smatch q;
                if (std::regex_match(name, q, status_re))
                    return {"STATUS" + q[1].str()};
                return pins.count(name) ? std::vector<std::string>{name} : std::vector<std::string>{};
            });
        }
    }

    // ------------------------------------------------------------------ block RAMs and multipliers
    // A RAMB16_S<w>[_S<w>] becomes a RAMB16, its pins as ISE's map writes them (Silinx's pack-bram.js): a port of w bits
    // on ADDR<P>(14 - n)..ADDR<P>13 (n address bits), DI<P>0.., DIP<P>0.. (port A alone: no letter in the cell's names).
    // A MULT18X18 / MULT18X18S / MULT18X18SIO becomes a MULT18X18SIO (Silinx's pack-dcm-mult.js): MULT18X18S's C on CLK,
    // CE on CEA / CEB / CEP, R on RSTA / RSTB / RSTP; a MULT18X18's enables and clock at 0, its resets at 1; its timing
    // variant by its registers. A constant on a pin: a GND source (a slice of its own) per cell, a VCC site per pin. The
    // undriven inputs (x) are left unconnected; the cascade (BCIN / BCOUT, B_INPUT = CASCADE) is refused.
    dict<std::pair<IdString, int>, NetInfo *> hard_consts;

    // Where the VCC site that reaches a pin of a block (`type`: RAMB16 / MULT18X18SIO) is, from the block's own BEL: the
    // BRAM<k>_SMALL tile beside the pin's row, whose BRAM_VCC_WIRE drives the pin's wire (the same at every block)
    dict<std::pair<IdString, IdString>, Loc> vcc_offsets;

    Loc vcc_offset(IdString type, IdString port)
    {
        auto key = std::make_pair(type, port);
        auto found = vcc_offsets.find(key);
        if (found != vcc_offsets.end())
            return found->second;
        BelId bel;
        for (BelId b : ctx->getBels())
            if (ctx->getBelType(b) == type) {
                bel = b;
                break;
            }
        Loc at = ctx->getBelLocation(bel), v = ctx->getBelLocation(vcc_near(bel, port));
        return vcc_offsets[key] = Loc(v.x - at.x, v.y - at.y, v.z);
    }

    // the VCC site that reaches a pin of a BEL
    BelId vcc_near(BelId bel, IdString port) const
    {
        std::vector<WireId> wires = {ctx->getBelPinWire(bel, port)};
        for (int hop = 0; hop < 3; hop++) {
            std::vector<WireId> up;
            for (WireId w : wires)
                for (PipId pip : ctx->getPipsUphill(w)) {
                    WireId src = ctx->getPipSrcWire(pip);
                    for (BelPin bp : ctx->getWireBelPins(src))
                        if (ctx->getBelType(bp.bel) == ctx->id("VCCSRC"))
                            return bp.bel;
                    up.push_back(src);
                }
            wires = up;
        }
        log_error("%s pin %s: no VCC site reaches it\n", ctx->nameOfBel(bel), port.c_str(ctx));
    }

    NetInfo *hard_const(CellInfo *ci, int v, IdString port)
    {
        // (a DCM is placed by now, and the sites differ in shape: its 1s from the VCC site of its own tile, fixed there)
        if (v && ci->type == id_DCM) {
            BelId vcc = vcc_near(ctx->getBelByNameStr(ci->attrs.at(id_BEL).as_string()), port);
            Loc l = ctx->getBelLocation(vcc);
            auto key = std::make_pair(ci->name, 1 + l.x * 4096 + l.y * 64 + l.z);
            auto found = hard_consts.find(key);
            if (found != hard_consts.end())
                return found->second;
            IdString name = ctx->id(ci->name.str(ctx) + "$vcc");
            CellInfo *src = ctx->createCell(name, ctx->id("VCCSRC"));
            src->addOutput(id_O);
            src->attrs[id_BEL] = std::string(ctx->nameOfBel(vcc));
            NetInfo *n = ctx->createNet(ctx->idf("%s$o", name.c_str(ctx)));
            src->connectPort(id_O, n);
            return hard_consts[key] = n;
        }
        // (a slice's Y alone, for the cell's 0s; for its 1s the VCC site beside each pin, which reaches nothing else, in
        // the cell's cluster: XDL's gnd / vcc nets, which ISE takes for constants)
        Loc off = v ? vcc_offset(ci->type, port) : Loc();
        auto key = std::make_pair(ci->name, v ? 1 + off.x * 4096 + off.y * 64 + off.z : 0);
        auto found = hard_consts.find(key);
        if (found != hard_consts.end())
            return found->second;
        IdString name = v ? ctx->idf("%s$vcc_%d_%d", ci->name.c_str(ctx), off.y, off.z) : ctx->id(ci->name.str(ctx) + "$gnd");
        CellInfo *src = ctx->createCell(name, ctx->id(v ? "VCCSRC" : "GNDSRC"));
        src->addOutput(id_O);
        NetInfo *n = ctx->createNet(ctx->idf("%s$o", name.c_str(ctx)));
        src->connectPort(id_O, n);
        if (v) {
            ci->cluster = ci->name;
            ci->constr_children.push_back(src);
            src->cluster = ci->name;
            src->constr_x = off.x;
            src->constr_y = off.y;
            src->constr_z = off.z;
            src->constr_abs_z = true;
        }
        hard_consts[key] = n;
        return n;
    }

    // the cell's ports renamed (`to`: the new name of a port, empty to drop it); a constant input on its shared LUT
    void hard_ports(CellInfo *ci, std::function<std::vector<std::string>(const std::string &)> to)
    {
        std::vector<std::tuple<IdString, PortType, NetInfo *>> old;
        for (auto &port : ci->ports)
            old.emplace_back(port.first, port.second.type, port.second.net);
        for (auto &o : old) {
            ci->disconnectPort(std::get<0>(o));
            ci->ports.erase(std::get<0>(o));
        }
        for (auto &o : old) {
            auto [name, type, net] = o;
            for (auto &n : to(name.str(ctx))) {
                IdString port = ctx->id(n);
                if (type == PORT_IN) {
                    if (!net || !net->driver.cell)
                        continue;   // (undriven: x)
                    if (net->driver.cell->type.in(id_GND, id_VCC))
                        net = hard_const(ci, net->driver.cell->type == id_VCC, port);
                    ci->addInput(port);
                } else {
                    if (!net || net->users.entries() == 0)
                        continue;
                    ci->addOutput(port);
                }
                ci->connectPort(port, net);
            }
        }
    }

    void tie_hard(CellInfo *ci, const char *port, int v)
    {
        IdString p = ctx->id(port);
        ci->addInput(p);
        ci->connectPort(p, hard_const(ci, v, p));
    }

    void pack_hard()
    {
        static const std::regex bram_re("RAMB16_S(1|2|4|9|18|36)(_S(1|2|4|9|18|36))?");
        static const std::regex mult_re("MULT18X18(S|SIO)?");
        static const std::regex pin_re("([A-Z]+?)([AB])?(\\[(\\d+)\\])?");
        static const std::regex mpin_re("(A|B|P|BCIN|BCOUT)(\\[(\\d+)\\])?");
        static const std::map<int, int> addr_bits = {{1, 14}, {2, 13}, {4, 12}, {9, 11}, {18, 10}, {36, 9}};
        std::vector<CellInfo *> cells;
        for (auto &cell : ctx->cells)
            cells.push_back(cell.second.get());
        for (CellInfo *ci : cells) {
            std::string type = ci->type.str(ctx);
            std::smatch m;
            if (std::regex_match(type, m, bram_re)) {
                bool single = !m[2].matched;
                std::map<char, int> width = {{'A', std::stoi(m[1])}};
                if (!single)
                    width['B'] = std::stoi(m[3]);
                ci->type = ctx->id("RAMB16");   // (first: the VCC sites of its pins, vcc_offset)
                hard_ports(ci, [&](const std::string &name) -> std::vector<std::string> {
                    std::smatch q;
                    if (!std::regex_match(name, q, pin_re))
                        return {};
                    std::string base = q[1], idx = q[4];
                    static const std::set<std::string> known = {"DOP", "DIP", "DO", "DI", "ADDR", "CLK", "EN", "SSR", "WE"};
                    if (!known.count(base))
                        return {};
                    char port = single ? 'A' : (q[2].matched ? q[2].str()[0] : 0);
                    if (!width.count(port) || (single && q[2].matched))
                        return {};
                    int i = idx.empty() ? 0 : std::stoi(idx);
                    if (base == "ADDR")
                        return {stringf("ADDR%c%d", port, 14 - addr_bits.at(width[port]) + i)};
                    if (base == "DO" || base == "DI" || base == "DOP" || base == "DIP")
                        return {stringf("%s%c%d", base.c_str(), port, i)};
                    return {stringf("%s%c", base.c_str(), port)};
                });
                for (auto &w : width)
                    ci->params[ctx->idf("PORT%c_WIDTH", w.first)] = Property(w.second, 32);
                ci->params[ctx->id("SILINX_TYPE")] = std::string(type);
            } else if (std::regex_match(type, mult_re)) {
                auto param = [&](const char *k, int dflt) {
                    auto f = ci->params.find(ctx->id(k));
                    return f == ci->params.end() ? dflt : (f->second.is_string ? std::stoi(f->second.as_string()) : f->second.as_int64());
                };
                auto b_input = ci->params.find(ctx->id("B_INPUT"));
                if (b_input != ci->params.end() && b_input->second.as_string().rfind("CASCADE", 0) == 0)
                    log_error("%s: a multiplier's cascade (B_INPUT = CASCADE) is not in the spartan3e micro-architecture yet\n", ctx->nameOf(ci));
                bool sio = type == "MULT18X18SIO", s = type == "MULT18X18S";
                ci->type = ctx->id("MULT18X18SIO");
                hard_ports(ci, [&](const std::string &name) -> std::vector<std::string> {
                    std::smatch q;
                    if (std::regex_match(name, q, mpin_re)) {
                        if (q[1] == "BCIN" || q[1] == "BCOUT")
                            return {};
                        return {q[1].str() + (q[3].matched ? q[3].str() : "0")};
                    }
                    if (s) {
                        if (name == "C")
                            return {"CLK"};
                        if (name == "CE")
                            return {"CEA", "CEB", "CEP"};
                        if (name == "R")
                            return {"RSTA", "RSTB", "RSTP"};
                        return {};
                    }
                    static const std::set<std::string> ctl = {"CLK", "CEA", "CEB", "CEP", "RSTA", "RSTB", "RSTP"};
                    return sio && ctl.count(name) ? std::vector<std::string>{name} : std::vector<std::string>{};
                });
                for (auto &port : ci->ports)
                    if (port.first.str(ctx).rfind("BCOUT", 0) == 0)
                        log_error("%s: a multiplier's cascade (BCOUT) is not in the spartan3e micro-architecture yet\n", ctx->nameOf(ci));
                if (type == "MULT18X18") {
                    for (const char *p : {"CEA", "CEB", "CEP", "CLK"})
                        tie_hard(ci, p, 0);
                    for (const char *p : {"RSTA", "RSTB", "RSTP"})
                        tie_hard(ci, p, 1);
                }
                int areg = sio ? param("AREG", 1) : 0, breg = sio ? param("BREG", 1) : 0, preg = sio ? param("PREG", 1) : s ? 1 : 0;
                ci->params[ctx->id("AREG")] = Property(areg, 32);
                ci->params[ctx->id("BREG")] = Property(breg, 32);
                ci->params[ctx->id("PREG")] = Property(preg, 32);
                ci->params[ctx->id("SILINX_TYPE")] = std::string(type);
                IdString variant = ctx->id(areg || breg ? "MULT_IREG" : preg ? "MULT_PREG" : "MULT_COMB");
                if (ctx->speed_grade)
                    ci->timing_index = ctx->get_cell_timing_idx(variant);
            }
        }
    }

    // ------------------------------------------------------------------ wide multiplexers
    // A MUXF5 takes its slice's two LUTs (I1 on F, I0 on G) and BX. A tree of MUXF5 .. MUXF8 takes the dedicated F5 / FX
    // wires, as Silinx's packer (fpga/pack-muxes.js, its WIDE_MUX): each input of a MUXF6 / F7 / F8 the output of the
    // level below, nothing else claiming it. A slice is (column, row) in a CLB's four: a MUXF6 at (c, 0) reads (I1) the F5
    // at (c, 0) and (I0) at (c, 1); a MUXF7 at (0, 1) the F6s at (0, 0) and (1, 0); a MUXF8 at (1, 1) the F7s at (0, 1) and
    // (0, 3), the CLB above. One cluster, from the tree's top; any other multiplexer becomes a LUT, O = S ? I1 : I0.

    // a multiplexer as a LUT4: I0, I1, S on I2
    void mux_to_lut(CellInfo *m)
    {
        m->renamePort(id_S, id_I2);
        m->params[id_INIT] = Property(0xCACA, 16);
        m->type = id_LUT4;
    }

    // the LUT on a MUXF5's input: the net's driver when a LUT4 feeding only this input, a copy of it when it feeds others,
    // else a LUT passing the net through
    CellInfo *input_lut(CellInfo *m, IdString port)
    {
        NetInfo *n = m->getPort(port);
        CellInfo *d = n ? n->driver.cell : nullptr;
        if (d && d->type == id_LUT4 && d->cluster == ClusterId() && n->users.entries() == 1)
            return d;
        CellInfo *lut;
        IdString name = ctx->idf("%s$%s", m->name.c_str(ctx), port.c_str(ctx));
        NetInfo *o = d && d->type == id_LUT4 ? copy_lut(d, name, &lut) : pass_lut(n, name, &lut);
        if (!n)
            lut->params[id_INIT] = Property(0, 16);
        m->disconnectPort(port);
        m->connectPort(port, o);
        return lut;
    }

    // the flip-flop a multiplexer's output feeds (D), when not in a cluster yet
    CellInfo *output_ff(CellInfo *m)
    {
        if (NetInfo *o = m->getPort(id_O))
            for (auto &usr : o->users)
                if (usr.cell->type == id_FF && usr.port == id_D && usr.cell->cluster == ClusterId())
                    return usr.cell;
        return nullptr;
    }

    // a cluster rooted at `root`, its other cells at z relative to it
    void make_cluster(CellInfo *root, const std::vector<std::pair<CellInfo *, int>> &cells)
    {
        root->cluster = root->name;
        root->constr_abs_z = false;
        for (auto [c, dz] : cells) {
            root->constr_children.push_back(c);
            c->cluster = root->name;
            c->constr_x = 0;
            c->constr_y = 0;
            c->constr_z = dz;
            c->constr_abs_z = false;
        }
    }

    // a MUXF5's cells at z relative to its F LUT: (its F LUT, then the others)
    std::vector<std::pair<CellInfo *, int>> f5_cells(CellInfo *m)
    {
        CellInfo *f = input_lut(m, id_I1), *g = input_lut(m, id_I0);
        return {{f, 0}, {g, Z_GLUT - Z_FLUT}, {m, Z_F5MUX - Z_FLUT}};
    }

    static int mux_level(const CellInfo *m)
    {
        return m->type == id_MUXF5 ? 5 : m->type == id_MUXF6 ? 6 : m->type == id_MUXF7 ? 7 : m->type == id_MUXF8 ? 8 : 0;
    }

    // the tree under m, or false (nothing claimed): its multiplexers, I1's side first
    bool mux_tree(CellInfo *m, const pool<IdString> &claimed, std::vector<CellInfo *> &tree)
    {
        tree.push_back(m);
        if (mux_level(m) == 5)
            return true;
        for (IdString port : {id_I1, id_I0}) {
            NetInfo *n = m->getPort(port);
            CellInfo *d = n ? n->driver.cell : nullptr;
            if (!d || mux_level(d) != mux_level(m) - 1 || claimed.count(d->name) ||
                std::find(tree.begin(), tree.end(), d) != tree.end() || !mux_tree(d, claimed, tree))
                return false;
        }
        return true;
    }

    // a tree's cells from the multiplexer m in slice (col, row): each at z and y relative to the top one's FXMUX
    void place_mux(CellInfo *m, int col, int row, int top_col, int top_row, std::vector<std::tuple<CellInfo *, int, int>> &out)
    {
        int y = -(row >> 1) + (top_row >> 1);   // (a slice's row up: the tile's y down)
        auto site = [](int c, int r) { return 2 * c + (r & 1); };
        int dz = (site(col, row) - site(top_col, top_row)) * Z_STRIDE - Z_FXMUX;
        if (mux_level(m) == 5) {
            for (auto [c, z] : f5_cells(m))
                out.emplace_back(c, y, dz + Z_FLUT + z);
            return;
        }
        if (col != top_col || row != top_row)   // (not the top itself: the cluster's root)
            out.emplace_back(m, y, dz + Z_FXMUX);
        int l = mux_level(m);
        CellInfo *a = m->getPort(id_I1)->driver.cell, *b = m->getPort(id_I0)->driver.cell;
        if (l == 6) {
            place_mux(a, col, row, top_col, top_row, out);
            place_mux(b, col, row + 1, top_col, top_row, out);
        } else if (l == 7) {
            place_mux(a, col, row - 1, top_col, top_row, out);
            place_mux(b, col + 1, row - 1, top_col, top_row, out);
        } else {
            place_mux(a, col - 1, row, top_col, top_row, out);
            place_mux(b, col - 1, row + 2, top_col, top_row, out);
        }
    }

    void pack_muxes()
    {
        std::vector<CellInfo *> levels[9];
        for (auto &cell : ctx->cells)
            if (int l = mux_level(cell.second.get()))
                levels[l].push_back(cell.second.get());
        pool<IdString> claimed;
        int trees[9] = {0}, luts = 0;
        for (int l : {8, 7, 6}) {
            for (CellInfo *m : levels[l]) {
                if (claimed.count(m->name))
                    continue;
                std::vector<CellInfo *> tree;
                if (!mux_tree(m, claimed, tree)) {
                    mux_to_lut(m);
                    luts++;
                    continue;
                }
                for (CellInfo *t : tree)
                    claimed.insert(t->name);
                // the top's place: an F6 at (0, 0), an F7 at (0, 1), an F8 at (1, 1)
                int col = l == 8, row = l != 6;
                std::vector<std::tuple<CellInfo *, int, int>> cells;
                place_mux(m, col, row, col, row, cells);
                if (CellInfo *ff = output_ff(m))
                    cells.emplace_back(ff, 0, Z_FFY - Z_FXMUX);
                m->cluster = m->name;
                m->constr_abs_z = false;
                for (auto [c, y, dz] : cells) {
                    m->constr_children.push_back(c);
                    c->cluster = m->name;
                    c->constr_x = 0;
                    c->constr_y = y;
                    c->constr_z = dz;
                    c->constr_abs_z = false;
                }
                trees[l]++;
            }
        }
        for (CellInfo *m : levels[5]) {
            if (claimed.count(m->name))
                continue;
            auto cells = f5_cells(m);
            if (CellInfo *ff = output_ff(m))
                cells.emplace_back(ff, Z_FFX - Z_FLUT);
            CellInfo *root = cells.front().first;
            cells.erase(cells.begin());
            make_cluster(root, cells);
            trees[5]++;
        }
        if (trees[5] + trees[6] + trees[7] + trees[8] + luts)
            log_info("Packed %d MUXF8, %d MUXF7, %d MUXF6 and %d MUXF5 cluster(s), %d wide multiplexer(s) as LUTs.\n", trees[8], trees[7],
                     trees[6], trees[5], luts);
    }

    // ------------------------------------------------------------------ distributed RAMs and shift registers
    // On a SLICEM, as Silinx's packer (fpga/pack-mems.js), so as ISE's map. Each LUT used is a MEM16: its address on the
    // site's pins, the write clock on CLK, the write or shift enable on SR (none: tied to 1), the data on BX / BY, or the
    // stage before through SHIFTIN.
    //   RAM16X1S, SRL16*: G, then F for another of the same controls (a RAM: and the same address);
    //   RAM16X1D: G the write port and SPO, F read at DPRA (DPO), written at G's address;
    //   RAM32X1S: G addresses 0-15, F 16-31, A4 on BX, O an F5 of the two;
    //   a shift register read at its last stage into another's D: G then F, on SHIFTIN;
    //   SRLC16E chained through Q15: up to four, G and F of X0Y1 of a CLB, then of X0Y0 (SHIFTOUT -> SHIFTIN).
    // A slice is one cluster, from its first G. A flip-flop of the same clock, with no enable and no set / reset, joins its
    // shift register's side when the shift is always enabled.
    static uint64_t param_bits(const CellInfo *c, IdString p)
    {
        auto it = c->params.find(p);
        return it == c->params.end() ? 0 : uint64_t(it->second.as_int64());
    }

    struct MemCell
    {
        CellInfo *c;
        NetInfo *clk, *en;
        bool inv, srl;
    };

    bool is_mem_type(IdString t) const
    {
        static const std::regex re("(RAM16X1S|RAM16X1D|RAM32X1S|SRLC?16E?(_1)?)");
        return std::regex_match(t.str(ctx), re);
    }

    // a MEM16 of `from`'s controls: its address nets, data, output, contents
    CellInfo *new_mem(const std::string &name, CellInfo *from, const char *addr, NetInfo *d, NetInfo *o, uint64_t init, const char *mode)
    {
        CellInfo *m = ctx->createCell(ctx->id(name), id_MEM16);
        for (int k = 0; k < 4; k++) {
            m->addInput(ctx->idf("A%d", k));
            if (NetInfo *a = from->getPort(ctx->idf("%s%d", addr, k)))
                m->connectPort(ctx->idf("A%d", k), a);
        }
        for (IdString p : {id_D, id_CLK, id_SR})
            m->addInput(p);
        m->addOutput(id_O);
        if (d)
            m->connectPort(id_D, d);
        m->connectPort(id_CLK, from->getPort(id_CLK));
        m->connectPort(id_SR, from->getPort(id_SR));
        if (o)
            m->connectPort(id_O, o);
        m->params[id_INIT] = Property(init & 0xFFFF, 16);
        m->params[id_MODE] = std::string(mode);
        m->params[id_CLKINV] = from->params.at(id_CLKINV);
        return m;
    }

    void pack_mems()
    {
        std::vector<MemCell> mems;
        dict<IdString, int> index;
        for (auto &cell : ctx->cells) {
            CellInfo *c = cell.second.get();
            if (!is_mem_type(c->type))
                continue;
            std::string t = c->type.str(ctx);
            bool srl = t[0] == 'S';
            const char *pin = srl ? "CLK" : "WCLK";
            bool inv = (t.size() > 2 && t.substr(t.size() - 2) == "_1") != (param_bits(c, ctx->idf("IS_%s_INVERTED", pin)) & 1);
            if (!srl)
                rename_port(c, "WCLK", id_CLK);
            rename_port(c, srl ? "CE" : "WE", id_SR);
            if (srl)
                rename_port(c, "Q", id_O);
            c->params[id_CLKINV] = Property(inv ? 1 : 0, 1);
            NetInfo *en = c->getPort(id_SR);
            if (en && is_constant(en, id_VCC)) {
                c->disconnectPort(id_SR);
                en = nullptr;
            }
            index[c->name] = mems.size();
            mems.push_back(MemCell{c, c->getPort(id_CLK), en, inv, srl});
        }
        if (mems.empty())
            return;
        auto same_key = [](const MemCell &a, const MemCell &b) { return a.clk == b.clk && a.en == b.en && a.inv == b.inv && a.srl == b.srl; };
        // the slice's enable when it has none, and the address bits a cell leaves out
        auto tie = [&](CellInfo *m) {
            if (!m->ports.count(id_SR))
                m->addInput(id_SR);
            if (!m->getPort(id_SR))
                m->connectPort(id_SR, constant_net(ctx->idf("%s$en", m->name.c_str(ctx)), id_VCC));
        };
        auto as_mem = [&](CellInfo *c, const char *mode) {
            c->type = id_MEM16;
            c->params[id_MODE] = std::string(mode);
            c->params[id_INIT] = Property(param_bits(c, id_INIT) & 0xFFFF, 16);
            if (mode[0] == 'S')
                c->timing_index = ctx->get_cell_timing_idx(ctx->id("MEM16S"));
            for (int k = 0; k < 4; k++)
                if (!c->ports.count(ctx->idf("A%d", k)))
                    c->addInput(ctx->idf("A%d", k));
        };
        // the next shift register through Q15, if any
        auto q15_next = [&](const MemCell &m) -> int {
            NetInfo *n = m.c->getPort(ctx->id("Q15"));
            if (!n || n->users.entries() == 0)
                return -1;
            PortRef u = *n->users.begin();
            auto it = index.find(u.cell->name);
            if (n->users.entries() != 1 || u.port != id_D || it == index.end() || !same_key(m, mems[it->second]))
                log_error("%s: the cascade output Q15 into anything but the D of one shift register of the same clock and enable is not supported.\n", ctx->nameOf(m.c));
            return it->second;
        };
        std::vector<bool> used(mems.size(), false), has_prev(mems.size(), false);
        std::vector<int> next(mems.size(), -1);
        for (int i = 0; i < int(mems.size()); i++)
            if (mems[i].srl && (next[i] = q15_next(mems[i])) >= 0)
                has_prev[next[i]] = true;
        std::vector<CellInfo *> dead;
        // the flip-flop an always shifting side's output feeds
        auto side_ff = [&](const MemCell &m, CellInfo *c) -> CellInfo * {
            NetInfo *o = c->getPort(id_O);
            if (!o || !m.srl || m.en || m.inv)
                return nullptr;
            for (auto &usr : o->users)
                if (usr.cell->type == id_FF && usr.port == id_D && usr.cell->cluster == ClusterId() && usr.cell->getPort(id_CLK) == m.clk &&
                    !usr.cell->getPort(id_CE) && !usr.cell->getPort(id_SR))
                    return usr.cell;
            return nullptr;
        };
        int slices = 0;
        // one slice: its G and F (either side's flip-flop with it), `dz` from the cluster's root
        auto slice = [&](std::vector<std::pair<CellInfo *, int>> &cells, const MemCell &m, CellInfo *g, CellInfo *f, int dz) {
            for (auto [c, zm, zf] : {std::make_tuple(g, Z_GMEM, Z_FFY), std::make_tuple(f, Z_FMEM, Z_FFX)}) {
                if (!c)
                    continue;
                tie(c);
                // (a side's own BEL type: a slice's first memory on G, as the data's multiplexers want it)
                if (c->timing_index == -1)
                    c->timing_index = ctx->get_cell_timing_idx(id_MEM16);
                c->type = zm == Z_GMEM ? id_MEM16G : id_MEM16F;
                cells.emplace_back(c, dz + zm - Z_GMEM);
                if (CellInfo *ff = side_ff(m, c)) {
                    ff->cluster = g->name;   // (taken: not another side's)
                    cells.emplace_back(ff, dz + zf - Z_GMEM);
                }
            }
            slices++;
        };
        auto cluster = [&](std::vector<std::pair<CellInfo *, int>> &cells) {
            CellInfo *root = cells.front().first;
            cells.erase(cells.begin());
            make_cluster(root, cells);
        };
        for (int i = 0; i < int(mems.size()); i++) {
            if (has_prev[i] || next[i] < 0)
                continue;
            std::vector<int> chain;
            for (int k = i; k >= 0 && chain.size() <= 4; k = next[k])
                chain.push_back(k);
            if (chain.size() > 4)
                log_error("%s: a chain of more than 4 shift registers through Q15 is not supported (the two SLICEMs of a CLB).\n", ctx->nameOf(mems[i].c));
            std::vector<CellInfo *> c;
            for (int k : chain) {
                used[k] = true;
                as_mem(mems[k].c, "SHIFT");
                c.push_back(mems[k].c);
            }
            std::vector<std::pair<CellInfo *, int>> cells;
            slice(cells, mems[i], c[0], c[1], 0);
            if (c.size() > 2)
                slice(cells, mems[i], c[2], c.size() > 3 ? c[3] : nullptr, -Z_STRIDE);
            cluster(cells);
        }
        for (int i = 0; i < int(mems.size()); i++)
            if (has_prev[i] && !used[i])
                log_error("%s: a ring of shift registers through Q15 is not supported.\n", ctx->nameOf(mems[i].c));
        auto all_vcc = [&](CellInfo *c) {
            for (int k = 0; k < 4; k++)
                if (!is_constant(c->getPort(ctx->idf("A%d", k)), id_VCC))
                    return false;
            return true;
        };
        for (int i = 0; i < int(mems.size()); i++) {
            if (used[i])
                continue;
            // (the second of a cascade: with the first)
            // (a head's output may feed several: only the first of them is its second, as the head below chooses it)
            bool second = false;
            NetInfo *d = mems[i].c->getPort(id_D);
            for (int h = 0; h < int(mems.size()); h++) {
                if (h == i || used[h] || !mems[h].srl || !mems[i].srl || !same_key(mems[h], mems[i]) || !d || mems[h].c->getPort(id_O) != d || !all_vcc(mems[h].c))
                    continue;
                int first = -1;
                for (int k = 0; k < int(mems.size()) && first < 0; k++)
                    if (k != h && !used[k] && mems[k].srl && same_key(mems[k], mems[h]) && mems[k].c->getPort(id_D) == d)
                        first = k;
                second = second || first == i;
            }
            if (second)
                continue;
            used[i] = true;
            MemCell &m = mems[i];
            CellInfo *c = m.c;
            std::string t = c->type.str(ctx), name = c->name.str(ctx);
            std::vector<std::pair<CellInfo *, int>> cells;
            if (t == "RAM16X1D") {
                tie(c);
                uint64_t init = param_bits(c, id_INIT);
                NetInfo *spo = c->getPort(ctx->id("SPO")), *dpo = c->getPort(ctx->id("DPO"));
                c->disconnectPort(ctx->id("SPO"));
                c->disconnectPort(ctx->id("DPO"));
                CellInfo *g = new_mem(name + ".SLICEM_G", c, "A", c->getPort(id_D), spo, init, "DUAL");
                CellInfo *f = new_mem(name + ".SLICEM_F", c, "DPRA", nullptr, dpo, init, "DUAL");   // (its data: G's, DIF_MUX = ALTDIF)
                f->params[ctx->id("WMATE")] = g->name.str(ctx);
                dead.push_back(c);
                slice(cells, m, g, f, 0);
            } else if (t == "RAM32X1S") {
                tie(c);
                uint64_t init = param_bits(c, id_INIT);
                NetInfo *a4 = c->getPort(ctx->id("A4")), *o = c->getPort(id_O);
                c->disconnectPort(id_O);
                NetInfo *ng = ctx->createNet(ctx->id(name + ".g")), *nf = ctx->createNet(ctx->id(name + ".f"));
                CellInfo *g = new_mem(name + ".SLICEM_G", c, "A", c->getPort(id_D), ng, init, "RAM32G");
                CellInfo *f = new_mem(name + ".SLICEM_F", c, "A", nullptr, nf, init >> 16, "RAM32F");
                f->params[ctx->id("WMATE")] = g->name.str(ctx);
                for (CellInfo *x : {g, f}) {
                    x->addInput(ctx->id("A4"));
                    if (a4)
                        x->connectPort(ctx->id("A4"), a4);
                }
                CellInfo *m5 = ctx->createCell(ctx->id(name + "_F5"), id_MUXF5);
                for (IdString p : {id_I0, id_I1, id_S})
                    m5->addInput(p);
                m5->addOutput(id_O);
                m5->connectPort(id_I0, ng);
                m5->connectPort(id_I1, nf);
                if (a4)
                    m5->connectPort(id_S, a4);
                dead.push_back(c);
                slice(cells, m, g, f, 0);
                cells.emplace_back(m5, Z_F5MUX - Z_GMEM);
                if (o)
                    m5->connectPort(id_O, o);
            } else {
                const char *mode = m.srl ? "SHIFT" : "RAM";
                as_mem(c, mode);
                CellInfo *mate = nullptr;
                // a cascade: this one read at its last stage into another's D, on SHIFTIN
                NetInfo *o = c->getPort(id_O);
                if (m.srl && o && all_vcc(c))
                    for (int k = 0; k < int(mems.size()) && !mate; k++)
                        if (k != i && !used[k] && mems[k].srl && same_key(mems[k], m) && mems[k].c->getPort(id_D) == o) {
                            used[k] = true;
                            mate = mems[k].c;
                            as_mem(mate, mode);
                            NetInfo *q = ctx->createNet(ctx->idf("%s$q15", c->name.c_str(ctx)));
                            if (!c->ports.count(ctx->id("Q15")))
                                c->addOutput(ctx->id("Q15"));
                            c->connectPort(ctx->id("Q15"), q);
                            mate->disconnectPort(id_D);
                            mate->connectPort(id_D, q);
                        }
                for (int k = 0; k < int(mems.size()) && !mate; k++) {
                    if (k == i || used[k] || !same_key(mems[k], m) || mems[k].c->type.str(ctx) != t)
                        continue;
                    bool same = true;
                    for (int a = 0; a < 4 && !m.srl; a++)
                        same = same && mems[k].c->getPort(ctx->idf("A%d", a)) == c->getPort(ctx->idf("A%d", a));
                    if (same) {
                        used[k] = true;
                        mate = mems[k].c;
                        as_mem(mate, mode);
                    }
                }
                slice(cells, m, c, mate, 0);
            }
            cluster(cells);
        }
        for (CellInfo *c : dead) {
            for (auto &port : c->ports)
                if (port.second.net)
                    c->disconnectPort(port.first);
            ctx->cells.erase(c->name);
        }
        log_info("Packed %d memories into %d SLICEMs.\n", int(mems.size()), slices);
    }

    // ------------------------------------------------------------------ constants
    static bool is_constant(const NetInfo *n, IdString value)
    {
        return n && n->driver.cell && n->driver.cell->type == value;
    }

    // Constants the slice takes without a net: a LUT's input (folded into its INIT), a MUXCY's DI (CY0F / CY0G: 0 or 1,
    // the cell's CY0). (A flip-flop's enable at 1, its set / reset at 0: pack_luts_ffs.)
    void fold_constants()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_LUT4) {
                uint64_t w = ci->params.at(id_INIT).as_int64();
                for (int k = 0; k < 4; k++) {
                    IdString port = ctx->idf("I%d", k);
                    NetInfo *n = ci->getPort(port);
                    int v = is_constant(n, id_VCC) ? 1 : is_constant(n, id_GND) ? 0 : -1;
                    if (v < 0)
                        continue;
                    // input k fixed at v: each entry takes the one with that bit at v
                    uint64_t f = 0;
                    for (int i = 0; i < 16; i++)
                        f |= ((w >> ((i & ~(1 << k)) | (v << k))) & 1ULL) << i;
                    w = f;
                    ci->disconnectPort(port);
                }
                ci->params[id_INIT] = Property(w, 16);
            } else if (ci->type == id_MUXCY) {
                NetInfo *n = ci->getPort(id_DI);
                int v = is_constant(n, id_VCC) ? 1 : is_constant(n, id_GND) ? 0 : -1;
                if (v >= 0) {
                    ci->params[id_CY0] = std::string(v ? "1" : "0");
                    ci->disconnectPort(id_DI);
                }
            }
        }
    }

    // The other users of '0' and '1' (a chain's carry in through BX, a flip-flop's D, an output): a LUT giving 0 / 1,
    // placed and routed as any other. The constant cells left without users go.
    void constants_to_luts()
    {
        std::vector<IdString> trim;
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type != id_GND && ci->type != id_VCC)
                continue;
            NetInfo *n = nullptr;
            IdString out;
            for (auto &port : ci->ports)
                if (port.second.net)
                    n = port.second.net, out = port.first;
            if (!n || n->users.entries() == 0) {
                trim.push_back(cell.first);
                continue;
            }
            bool one = ci->type == id_VCC;
            ci->renamePort(out, id_O);
            ci->params[id_INIT] = Property(one ? 0xFFFF : 0, 16);
            ci->type = id_LUT4;
        }
        for (auto c : trim) {
            CellInfo *ci = ctx->cells.at(c).get();
            for (auto &port : ci->ports)
                if (port.second.net)
                    ctx->nets.erase(port.second.net->name);
            ctx->cells.erase(c);
        }
    }

    // ------------------------------------------------------------------ carry chains
    // A chain: its stages (a MUXCY and the XORCY on the same carry in, or an XORCY alone at the end), two to a slice (F
    // then G), up a column of slices (COUT -> CIN). Each stage's S / LI is its own LUT's output (a LUT passing the net
    // through when it is not one); the sum's flip-flop goes with it. The chain is a cluster whose placement follows the
    // carry wiring (getClusterPlacement).
    struct ChainCell
    {
        CellInfo *cell;
        int slice, z;
    };
    dict<IdString, std::vector<ChainCell>> chains;   // by the cluster's root
    mutable dict<BelId, BelId> next_carry_cache;

    // a LUT4 passing `in` through (I0), its output a new net
    NetInfo *pass_lut(NetInfo *in, IdString name, CellInfo **lut)
    {
        CellInfo *l = ctx->createCell(name, id_LUT4);
        l->addInput(id_I0);
        l->addOutput(id_O);
        l->params[id_INIT] = Property(0xAAAA, 16);
        NetInfo *out = ctx->createNet(ctx->idf("%s$o", name.c_str(ctx)));
        if (in)
            l->connectPort(id_I0, in);
        l->connectPort(id_O, out);
        *lut = l;
        return out;
    }

    // a copy of a LUT4, on the same inputs, its output a new net
    NetInfo *copy_lut(const CellInfo *d, IdString name, CellInfo **lut)
    {
        CellInfo *l = ctx->createCell(name, id_LUT4);
        for (IdString p : {id_I0, id_I1, id_I2, id_I3})
            if (d->ports.count(p)) {
                l->addInput(p);
                if (NetInfo *n = d->ports.at(p).net)
                    l->connectPort(p, n);
            }
        l->addOutput(id_O);
        l->params[id_INIT] = d->params.at(id_INIT);
        NetInfo *out = ctx->createNet(ctx->idf("%s$o", name.c_str(ctx)));
        l->connectPort(id_O, out);
        *lut = l;
        return out;
    }

    // The LUT of a stage, as Silinx's packer (fpga/pack-slices.js lutFor): the S / LI net's driver when a LUT not in a
    // cluster yet, feeding only this stage or with X / Y free for its other loads (the sum not used); else a copy of
    // that LUT; else a pass-through LUT. (A copy, or the LUT itself, costs no level of logic more.)
    CellInfo *stage_lut(CellInfo *mux, CellInfo *xr, const std::string &name)
    {
        NetInfo *s = mux ? mux->getPort(id_S) : xr->getPort(id_LI);
        CellInfo *d = s ? s->driver.cell : nullptr;
        bool is_lut = d && d->type == id_LUT4;
        bool others = false;
        if (s)
            for (auto &usr : s->users)
                if (usr.cell != mux && usr.cell != xr)
                    others = true;
        NetInfo *sum = xr ? xr->getPort(id_O) : nullptr;
        bool out_free = !sum || sum->users.empty();
        if (is_lut && d->cluster == ClusterId() && (!others || out_free))
            return d;
        CellInfo *lut;
        NetInfo *o = is_lut ? copy_lut(d, ctx->id(name + "$copy"), &lut) : pass_lut(s, ctx->id(name + "$s"), &lut);
        if (mux) {
            mux->disconnectPort(id_S);
            mux->connectPort(id_S, o);
        }
        if (xr) {
            xr->disconnectPort(id_LI);
            xr->connectPort(id_LI, o);
        }
        return lut;
    }

    // A stage bringing a chain's carry in (to `first` and its XORCY `xr`): a MUXCY whose S is a LUT giving 0, so that
    // it passes its DI, the carry in (a constant: its CY0; a net: on DI, through the LUT's pin 1). Returns the MUXCY.
    CellInfo *carry_in_stage(CellInfo *first, CellInfo *xr)
    {
        NetInfo *ci = first->getPort(id_CI);
        std::string name = first->name.str(ctx) + "$cin";
        CellInfo *m = ctx->createCell(ctx->id(name), id_MUXCY);
        m->addInput(id_CI);
        m->addInput(id_DI);
        m->addInput(id_S);
        m->addOutput(id_O);
        CellInfo *zero;
        NetInfo *s = pass_lut(nullptr, ctx->id(name + "$zero"), &zero);
        zero->params[id_INIT] = Property(0, 16);
        m->connectPort(id_S, s);
        IdString k = ci && ci->driver.cell && ci->driver.cell->type.in(id_GND, id_VCC) ? ci->driver.cell->type : IdString();
        if (k != IdString())
            m->params[id_CY0] = std::string(k == id_VCC ? "1" : "0");
        else if (ci) {
            m->connectPort(id_DI, ci);
            zero->connectPort(id_I0, ci);   // (DI through pin 1: the LUT reads it, its function still 0)
        }
        NetInfo *out = ctx->createNet(ctx->id(name + "$o"));
        m->connectPort(id_O, out);
        for (CellInfo *c : {first, xr})
            if (c) {
                c->disconnectPort(id_CI);
                c->connectPort(id_CI, out);
            }
        return m;
    }

    void pack_carry()
    {
        std::vector<CellInfo *> muxes, xors;
        for (auto &cell : ctx->cells) {
            if (cell.second->type == id_MUXCY)
                muxes.push_back(cell.second.get());
            if (cell.second->type == id_XORCY)
                xors.push_back(cell.second.get());
        }
        if (muxes.empty() && xors.empty())
            return;
        // A stage whose select is the constant 1 passes its carry in (O = CI) and no sum reads it: left out, its loads read
        // the carry in, as Silinx's packer (fpga/pack-prep.js): a stage of delay less, and a slice for every two.
        std::vector<CellInfo *> kept;
        for (CellInfo *m : muxes) {
            NetInfo *s = m->getPort(id_S), *ci = m->getPort(id_CI), *o = m->getPort(id_O);
            bool passes = s && s->driver.cell && s->driver.cell->type == id_VCC && ci && ci->driver.cell && !ci->driver.cell->type.in(id_GND, id_VCC);
            if (passes)
                for (auto &usr : ci->users)
                    if (usr.cell->type == id_XORCY && usr.port == id_CI)
                        passes = false;
            if (!passes) {
                kept.push_back(m);
                continue;
            }
            if (o) {
                std::vector<PortRef> users(o->users.begin(), o->users.end());
                for (auto &usr : users) {
                    usr.cell->disconnectPort(usr.port);
                    usr.cell->connectPort(usr.port, ci);
                }
            }
            for (IdString p : {id_S, id_CI, id_DI, id_O})
                m->disconnectPort(p);
            ctx->cells.erase(m->name);
        }
        muxes = kept;
        dict<IdString, CellInfo *> next;   // a MUXCY -> the MUXCY its O carries into
        pool<IdString> continued;
        for (CellInfo *m : muxes) {
            NetInfo *ci = m->getPort(id_CI);
            if (ci && ci->driver.cell && ci->driver.cell->type == id_MUXCY && !next.count(ci->driver.cell->name)) {
                next[ci->driver.cell->name] = m;
                continued.insert(m->name);
            }
        }
        pool<IdString> used_xor;
        // the XORCY on a stage's carry in (and its S, when given); a constant carry in (each '0' / '1' of the netlist a
        // net of its own): an XORCY on the same constant, with that S
        auto constant_of = [&](NetInfo *n) -> IdString {
            return n && n->driver.cell && n->driver.cell->type.in(id_GND, id_VCC) ? n->driver.cell->type : IdString();
        };
        auto take_xor = [&](NetInfo *ci, NetInfo *li) -> CellInfo * {
            if (!ci)
                return nullptr;
            auto ok = [&](CellInfo *x) { return !used_xor.count(x->name) && (!li || x->getPort(id_LI) == li); };
            for (auto &usr : ci->users)
                if (usr.cell->type == id_XORCY && usr.port == id_CI && ok(usr.cell)) {
                    used_xor.insert(usr.cell->name);
                    return usr.cell;
                }
            IdString k = constant_of(ci);
            if (k != IdString() && li)
                for (auto &usr : li->users)
                    if (usr.cell->type == id_XORCY && usr.port == id_LI && ok(usr.cell) && constant_of(usr.cell->getPort(id_CI)) == k) {
                        CellInfo *x = usr.cell;
                        used_xor.insert(x->name);
                        // (one net on the slice's carry in: the MUXCY's)
                        x->disconnectPort(id_CI);
                        x->connectPort(id_CI, ci);
                        return x;
                    }
            return nullptr;
        };
        std::vector<std::vector<std::pair<CellInfo *, CellInfo *>>> all;   // stages: (MUXCY, XORCY)
        for (CellInfo *m : muxes) {
            if (continued.count(m->name))
                continue;
            std::vector<std::pair<CellInfo *, CellInfo *>> stages;
            for (CellInfo *c = m; c; c = next.count(c->name) ? next.at(c->name) : nullptr)
                stages.emplace_back(c, take_xor(c->getPort(id_CI), c->getPort(id_S)));
            // The first stage's DI on BX (its LUT reads four other nets, so not pin 1), where the carry in comes too: a
            // stage before it brings the carry in (its LUT gives 0, so its MUXCY passes its DI: the carry in's constant,
            // or its net on pin 1), as Silinx's packer does
            if (NetInfo *di = m->getPort(id_DI)) {
                NetInfo *s = m->getPort(id_S);
                CellInfo *d = s ? s->driver.cell : nullptr;
                int inputs = 0;
                bool reads_di = false;
                if (d && d->type == id_LUT4)
                    for (int k = 0; k < 4; k++)
                        if (NetInfo *in = d->getPort(ctx->idf("I%d", k)))
                            inputs++, reads_di |= in == di;
                if (inputs == 4 && !reads_di)
                    stages.insert(stages.begin(), {carry_in_stage(m, stages.front().second), nullptr});
            }
            CellInfo *last = stages.back().first;
            NetInfo *lo = last->getPort(id_O);
            // (the carry out read by logic leaves by the slice's XB / YB, as ISE's map and Silinx's packer)
            if (CellInfo *x = take_xor(lo, nullptr))
                stages.emplace_back(nullptr, x);
            // a stage's carry out read by logic other than the chain: not yet
            for (size_t k = 0; k + 1 < stages.size(); k++) {
                NetInfo *o = stages[k].first ? stages[k].first->getPort(id_O) : nullptr;
                if (o)
                    for (auto &usr : o->users)
                        if (usr.port != id_CI || !usr.cell->type.in(id_MUXCY, id_XORCY))
                            log_error("the carry out of '%s' is read by '%s': not supported yet.\n", ctx->nameOf(stages[k].first),
                                      ctx->nameOf(usr.cell));
            }
            all.push_back(stages);
        }
        for (CellInfo *x : xors)
            if (!used_xor.count(x->name)) {
                used_xor.insert(x->name);
                all.push_back({{nullptr, x}});
            }
        int stages_n = 0;
        for (auto &stages : all) {
            std::vector<ChainCell> cells;
            for (int k = 0; k < int(stages.size()); k++) {
                auto [mux, xr] = stages[k];
                int slice = k / 2, side = k % 2;
                CellInfo *lut = stage_lut(mux, xr, (mux ? mux : xr)->name.str(ctx));
                cells.push_back({lut, slice, Z_FLUT + side});
                if (mux)
                    cells.push_back({mux, slice, Z_CYMUXF + side});
                if (xr) {
                    cells.push_back({xr, slice, Z_XORF + side});
                    // the sum's flip-flop, beside it (when its control set suits the slice's other one)
                    NetInfo *sum = xr->getPort(id_O);
                    if (sum)
                        for (auto &usr : sum->users)
                            if (usr.cell->type == id_FF && usr.port == id_D && usr.cell->cluster == ClusterId()) {
                                const CellInfo *other = nullptr;
                                for (auto &c : cells)
                                    if (c.slice == slice && c.z == Z_FFX + (1 - side))
                                        other = c.cell;
                                if (!same_control(usr.cell, other))
                                    continue;
                                cells.push_back({usr.cell, slice, Z_FFX + side});
                                break;
                            }
                }
                stages_n++;
            }
            CellInfo *root = cells.front().cell;   // the first stage's LUT, on F
            root->cluster = root->name;
            root->constr_abs_z = false;
            for (size_t i = 1; i < cells.size(); i++) {
                CellInfo *c = cells[i].cell;
                root->constr_children.push_back(c);
                c->cluster = root->name;
                // (an estimate for the placer's spreading: the slices two to a CLB up the column; getClusterPlacement
                // gives the real places)
                c->constr_x = 0;
                c->constr_y = -(cells[i].slice / 2);
                c->constr_z = (cells[i].slice % 2) * Z_STRIDE + cells[i].z - Z_FLUT;
                c->constr_abs_z = false;
            }
            chains[root->name] = cells;
        }
        log_info("Packed %d carry chain(s) of %d stage(s).\n", int(all.size()), stages_n);
    }

    // The CYMUXF of the slice a slice's CYMUXG carries into: COUT -> CIN (a PIP), CIN -> the CYINIT (a PIP of the slice).
    BelId next_carry(BelId cymuxg) const
    {
        auto it = next_carry_cache.find(cymuxg);
        if (it != next_carry_cache.end())
            return it->second;
        BelId found;
        WireId cout = ctx->getBelPinWire(cymuxg, id_O);
        for (PipId p1 : ctx->getPipsDownhill(cout)) {
            for (PipId p2 : ctx->getPipsDownhill(ctx->getPipDstWire(p1)))
                for (auto bp : ctx->getWireBelPins(ctx->getPipDstWire(p2)))
                    if (ctx->getBelType(bp.bel) == id_MUXCY && bp.pin == id_CI && ctx->getBelLocation(bp.bel).z % Z_STRIDE == Z_CYMUXF)
                        found = bp.bel;
            if (found != BelId())
                break;
        }
        next_carry_cache[cymuxg] = found;
        return found;
    }

    bool getClusterPlacement(ClusterId cluster, BelId root_bel, std::vector<std::pair<CellInfo *, BelId>> &placement) const override
    {
        auto it = chains.find(cluster);
        if (it == chains.end())
            return HimbaechelAPI::getClusterPlacement(cluster, root_bel, placement);
        Loc root = ctx->getBelLocation(root_bel);
        if (root.z % Z_STRIDE != Z_FLUT || ctx->getBelType(root_bel) != id_LUT4)
            return false;
        int n = 0;
        for (auto &c : it->second)
            n = std::max(n, c.slice + 1);
        std::vector<Loc> slices{Loc(root.x, root.y, root.z - Z_FLUT)};
        while (int(slices.size()) < n) {
            Loc l = slices.back();
            BelId g = ctx->getBelByLocation(Loc(l.x, l.y, l.z + Z_CYMUXG));
            BelId f = g == BelId() ? BelId() : next_carry(g);
            if (f == BelId())
                return false;
            Loc lf = ctx->getBelLocation(f);
            slices.push_back(Loc(lf.x, lf.y, lf.z - Z_CYMUXF));
        }
        placement.clear();
        for (auto &c : it->second) {
            Loc l = slices[c.slice];
            BelId b = ctx->getBelByLocation(Loc(l.x, l.y, l.z + c.z));
            if (b == BelId() || !ctx->isValidBelForCellType(c.cell->type, b))
                return false;
            placement.emplace_back(c.cell, b);
        }
        return true;
    }

    // ------------------------------------------------------------------ slices
    // As Silinx's packer (fpga/pack.js connectionCriticality, fpga/pack-luts.js). The criticality (0..1) of every
    // connection, from the cells only, before placement: the Spartan-3E -4's logic delays and a fixed delay for every
    // connection between cells, but those inside a slice. crit = 1 - slack / (the critical path's delay). Paths from
    // flip-flop to flip-flop; a design without them: from its inputs to its outputs.
    static constexpr double CRIT_NET = 0.45, CRIT_PAIR = 0.8, CRIT_BONUS = 8, CRIT_SLICES = 0.02, NO_PATH = -1e18;
    typedef dict<std::pair<IdString, IdString>, double> Criticality;

    static bool has_prefix(const std::string &s, const char *p) { return s.rfind(p, 0) == 0; }

    Criticality connection_criticality() const
    {
        const IdString ramb = ctx->id("RAMB16"), mult = ctx->id("MULT18X18SIO");
        auto registered = [&](const CellInfo *c) {
            auto it = c->params.find(ctx->id("PREG"));
            return c->type == mult && it != c->params.end() && it->second.as_int64() != 0;
        };
        auto is_data = [&](IdString p) {   // a multiplier's operand
            std::string s = p.str(ctx);
            return s.size() >= 2 && (s[0] == 'A' || s[0] == 'B') && isdigit(s[1]);
        };
        // (a connection always inside a slice: no routing delay)
        auto inside = [&](const CellInfo *c, IdString p, const CellInfo *d) {
            IdString t = c->type, dt = d->type;
            if (t == id_MUXF5)
                return p.in(id_I0, id_I1) && dt == id_LUT4;
            if (t.in(id_MUXF6, id_MUXF7, id_MUXF8))
                return p.in(id_I0, id_I1) && dt.in(id_MUXF5, id_MUXF6, id_MUXF7);
            if (t == id_MUXCY)
                return (p == id_CI && dt == id_MUXCY) || (p == id_S && dt == id_LUT4);
            if (t == id_XORCY)
                return (p == id_CI && dt == id_MUXCY) || (p == id_LI && dt == id_LUT4);
            return t == id_FF && p == id_D && dt.in(id_LUT4, id_MUXF5, id_MUXF6, id_XORCY);
        };
        auto net_delay = [&](const CellInfo *c, IdString p, const NetInfo *n) {
            return n->driver.cell && inside(c, p, n->driver.cell) ? 0.0 : CRIT_NET;
        };
        // the delay through a combinational cell from an input (NO_PATH: none)
        auto through = [&](const CellInfo *c, IdString p) {
            IdString t = c->type;
            if (t == id_LUT4)
                return 0.70;
            if (t == id_MUXF5)
                return p == id_S ? 0.74 : 0.32;
            if (t.in(id_MUXF6, id_MUXF7, id_MUXF8))
                return p == id_S ? 0.85 : 0.45;
            if (t == id_MUXCY)
                return p == id_CI ? 0.12 : p == id_S ? 0.25 : 0.95;
            if (t == id_XORCY)
                return p == id_CI ? 0.46 : 0.15;
            if (t == mult && !registered(c) && is_data(p))
                return 4.9;
            return NO_PATH;
        };
        bool with_io = false;
        // the setup of an end pin (NO_PATH: not an end)
        auto setup = [&](const CellInfo *c, IdString p) {
            IdString t = c->type;
            if (t == id_FF)
                return p == id_D ? 0.13 : p == id_CE ? 0.55 : p == id_SR ? 0.91 : NO_PATH;
            if (t == ramb)
                return has_prefix(p.str(ctx), "CLK") || has_prefix(p.str(ctx), "SSR") ? NO_PATH : 0.4;
            if (t == mult && registered(c))
                return is_data(p) ? 5.2 : NO_PATH;
            if (t == id_IOB)
                return with_io ? 0.0 : NO_PATH;
            return NO_PATH;
        };
        auto is_start = [&](const CellInfo *c) {
            return !c->type.in(id_LUT4, id_MUXF5, id_MUXF6, id_MUXF7, id_MUXF8, id_MUXCY, id_XORCY) &&
                   !(c->type == mult && !registered(c));
        };
        auto start = [&](const CellInfo *c) {
            IdString t = c->type;
            if (t == id_FF)
                return 0.59;
            if (t == ramb)
                return 2.39;
            if (t == mult)
                return 1.0;
            if (t == id_IOB)
                return with_io ? 0.0 : NO_PATH;
            return NO_PATH;
        };
        // arrival on a net (its driver's output); a loop counts as no path
        std::unordered_map<const NetInfo *, double> arr;
        pool<IdString> busy;
        std::function<double(const NetInfo *)> arr_net = [&](const NetInfo *n) -> double {
            auto it = arr.find(n);
            if (it != arr.end())
                return it->second;
            const CellInfo *c = n->driver.cell;
            if (!c)
                return NO_PATH;
            double t = NO_PATH;
            if (is_start(c))
                t = start(c);
            else {
                if (busy.count(n->name))
                    return NO_PATH;
                busy.insert(n->name);
                for (auto &port : c->ports) {
                    if (port.second.type != PORT_IN || !port.second.net)
                        continue;
                    double d = through(c, port.first), a = arr_net(port.second.net);
                    if (d != NO_PATH && a != NO_PATH)
                        t = std::max(t, a + net_delay(c, port.first, port.second.net) + d);
                }
                busy.erase(n->name);
            }
            arr[n] = t;
            return t;
        };
        auto worst = [&] {
            arr.clear();
            double w = 0;
            for (auto &cell : ctx->cells)
                for (auto &port : cell.second->ports) {
                    if (port.second.type != PORT_IN || !port.second.net)
                        continue;
                    double s = setup(cell.second.get(), port.first), a = arr_net(port.second.net);
                    if (s != NO_PATH && a != NO_PATH)
                        w = std::max(w, a + net_delay(cell.second.get(), port.first, port.second.net) + s);
                }
            return w;
        };
        double dmax = worst();
        if (dmax <= 0) {
            with_io = true;
            dmax = worst();
        }
        Criticality crit;
        if (dmax <= 0)
            return crit;
        // required time on a net: the earliest of its loads' (an end: dmax - setup; a combinational cell: its output's
        // less the delay through it). INF: no path.
        const double INF = 1e18;
        std::unordered_map<const NetInfo *, double> req;
        std::function<double(const CellInfo *, IdString)> req_pin;
        std::function<double(const NetInfo *)> req_net = [&](const NetInfo *n) -> double {
            auto it = req.find(n);
            if (it != req.end())
                return it->second;
            req[n] = INF;   // (loops)
            double r = INF;
            for (auto &usr : n->users) {
                double q = req_pin(usr.cell, usr.port);
                if (q < INF)
                    r = std::min(r, q - net_delay(usr.cell, usr.port, n));
            }
            req[n] = r;
            return r;
        };
        req_pin = [&](const CellInfo *c, IdString p) -> double {
            double s = setup(c, p);
            if (s != NO_PATH)
                return dmax - s;
            double d = through(c, p);
            if (d == NO_PATH || is_start(c))
                return INF;
            double r = INF;
            for (auto &port : c->ports)
                if (port.second.type == PORT_OUT && port.second.net)
                    r = std::min(r, req_net(port.second.net));
            return r < INF ? r - d : INF;
        };
        for (auto &cell : ctx->cells)
            for (auto &port : cell.second->ports) {
                if (port.second.type != PORT_IN || !port.second.net)
                    continue;
                double a = arr_net(port.second.net), r = req_pin(cell.second.get(), port.first);
                if (a == NO_PATH || r >= INF)
                    continue;
                double slack = r - net_delay(cell.second.get(), port.first, port.second.net) - a;
                crit[std::make_pair(cell.first, port.first)] = std::max(0.0, std::min(1.0, 1 - slack / dmax));
            }
        return crit;
    }

    // A unit: a LUT and the flip-flop it feeds, or a LUT or a flip-flop alone (its D through BX / BY).
    struct Unit
    {
        CellInfo *lut = nullptr, *ff = nullptr;
        std::vector<NetInfo *> nets;
        std::vector<std::pair<int, double>> links;   // the critical connections with other units
        double crit = 0;                             // the unit's most critical connection
    };

    // A slice: its F side (F, FFX) and G side (G, FFY), whose flip-flops share CLK, CE, SR and the set / reset mode.
    struct Slice
    {
        CellInfo *lut[2] = {nullptr, nullptr}, *ff[2] = {nullptr, nullptr};
        bool full() const { return lut[0] && lut[1] && ff[0] && ff[1]; }
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

    // Can a unit go into a slice? Its LUT's and flip-flop's side(s); a LUT with its flip-flop on one side, a LUT or a
    // flip-flop alone on any side free of its kind.
    static bool fits(const Slice &s, const Unit &u, int &lut_side, int &ff_side)
    {
        lut_side = ff_side = -1;
        if (u.ff && !same_control(s.ff[0] ? s.ff[0] : s.ff[1], u.ff))
            return false;
        for (int side = 0; side < 2; side++) {
            if (u.lut && u.ff) {
                if (!s.lut[side] && !s.ff[side]) {
                    lut_side = ff_side = side;
                    return true;
                }
            } else if (u.lut ? !s.lut[side] : !s.ff[side]) {
                (u.lut ? lut_side : ff_side) = side;
                return true;
            }
        }
        return false;
    }

    // Slices filled greedily: a seed unit, the units on critical paths first, then the compatible units sharing the most
    // nets with the slice, the units of a critical connection with it first of all; when nothing is related, the next
    // unit that fits. A LUT on a critical path goes only with its critical connections (with unrelated logic, placement
    // would pull it away from its path).
    std::vector<Slice> fill_slices(std::vector<Unit> &units, const dict<IdString, int> &unit_of, const Criticality &crit,
                                   bool timing)
    {
        for (auto &u : units) {
            u.links.clear();
            u.crit = 0;
        }
        if (timing)
            for (auto &c : crit) {
                if (c.second < CRIT_PAIR)
                    continue;
                CellInfo *cell = ctx->cells.at(c.first.first).get();
                auto load = unit_of.find(cell->name);
                NetInfo *net = cell->getPort(c.first.second);
                CellInfo *d = net ? net->driver.cell : nullptr;
                auto src = d ? unit_of.find(d->name) : unit_of.end();
                if (load != unit_of.end())
                    units[load->second].crit = std::max(units[load->second].crit, c.second);
                if (src != unit_of.end())
                    units[src->second].crit = std::max(units[src->second].crit, c.second);
                if (load != unit_of.end() && src != unit_of.end() && load->second != src->second) {
                    units[load->second].links.emplace_back(src->second, c.second);
                    units[src->second].links.emplace_back(load->second, c.second);
                }
            }
        dict<IdString, std::vector<int>> net_units;
        for (int k = 0; k < int(units.size()); k++)
            for (NetInfo *n : units[k].nets)
                net_units[n->name].push_back(k);
        std::vector<int> order(units.size());
        for (int k = 0; k < int(units.size()); k++)
            order[k] = k;
        auto critical = [&](const Unit &u) { return u.crit >= CRIT_PAIR; };
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const Unit &x = units[a], &y = units[b];
            if (critical(x) != critical(y))
                return critical(x);
            if (critical(x) && x.crit != y.crit)
                return x.crit > y.crit;
            return (x.ff != nullptr) > (y.ff != nullptr);
        });
        std::vector<bool> done(units.size(), false);
        std::vector<Slice> slices;
        auto put = [&](Slice &s, int k) {
            int ls, fs;
            NPNR_ASSERT(fits(s, units[k], ls, fs));
            if (ls >= 0)
                s.lut[ls] = units[k].lut;
            if (fs >= 0)
                s.ff[fs] = units[k].ff;
            done[k] = true;
        };
        size_t first = 0;
        for (int seed : order) {
            if (done[seed])
                continue;
            slices.emplace_back();
            Slice &s = slices.back();
            put(s, seed);
            std::vector<int> in_slice{seed};
            while (!s.full()) {
                std::map<int, double> score, link;
                pool<IdString> slice_nets;
                for (int u : in_slice)
                    for (NetInfo *n : units[u].nets)
                        slice_nets.insert(n->name);
                for (IdString n : slice_nets)
                    for (int k : net_units[n])
                        if (!done[k])
                            score[k] += 1;
                for (int u : in_slice)
                    for (auto &l : units[u].links)
                        if (!done[l.first])
                            link[l.first] = std::max(link[l.first], l.second);
                for (auto &l : link)
                    score[l.first] += CRIT_BONUS * l.second;
                int best = -1, ls, fs;
                double best_score = 0;
                for (auto &c : score) {   // (by index: the lowest of equal scores)
                    if (c.second <= best_score)
                        continue;
                    const Unit &u = units[c.first];
                    if (u.lut && critical(u) && !link.count(c.first))
                        continue;
                    if (fits(s, u, ls, fs))
                        best = c.first, best_score = c.second;
                }
                if (best < 0) {
                    while (first < order.size() && done[order[first]])
                        first++;
                    for (size_t i = first; i < order.size() && best < 0; i++) {
                        const Unit &u = units[order[i]];
                        if (!done[order[i]] && (!u.lut || !critical(u)) && fits(s, u, ls, fs))
                            best = order[i];
                    }
                    if (best < 0)
                        break;
                }
                put(s, best);
                in_slice.push_back(best);
            }
        }
        return slices;
    }

    void pack_slices()
    {
        std::vector<Unit> units;
        dict<IdString, int> unit_of;
        // a flip-flop with its LUT: the LUT driving its D, not yet in a unit
        for (auto &cell : ctx->cells) {
            CellInfo *ff = cell.second.get();
            if (ff->type != id_FF || ff->cluster != ClusterId())   // (a carry chain's: in its cluster)
                continue;
            NetInfo *d = ff->getPort(id_D);
            CellInfo *lut = d ? d->driver.cell : nullptr;
            if (lut && (lut->type != id_LUT4 || unit_of.count(lut->name) || lut->cluster != ClusterId()))
                lut = nullptr;
            unit_of[ff->name] = units.size();
            if (lut)
                unit_of[lut->name] = units.size();
            Unit u;
            u.lut = lut;
            u.ff = ff;
            units.push_back(u);
        }
        for (auto &cell : ctx->cells)
            if (cell.second->type == id_LUT4 && !unit_of.count(cell.first) && cell.second->cluster == ClusterId()) {
                unit_of[cell.first] = units.size();
                Unit u;
                u.lut = cell.second.get();
                units.push_back(u);
            }
        // a unit's nets, for the affinity: without the clocks, and the nets of many loads
        for (auto &u : units) {
            pool<IdString> seen;
            for (CellInfo *c : {u.lut, u.ff})
                if (c)
                    for (auto &port : c->ports) {
                        NetInfo *n = port.second.net;
                        if (n && port.first != id_CLK && n->users.entries() < 64 && !seen.count(n->name)) {
                            seen.insert(n->name);
                            u.nets.push_back(n);
                        }
                    }
        }
        // the units of critical connections in one slice, unless that costs more slices than CRIT_SLICES allows
        Criticality crit = connection_criticality();
        std::vector<Slice> plain = fill_slices(units, unit_of, crit, false), timed = fill_slices(units, unit_of, crit, true);
        bool timing = timed.size() <= plain.size() + std::max(1, int(CRIT_SLICES * plain.size()));
        // the clusters: F 0, G 1, FFX 2, FFY 3 within the slice, from the cell of the lowest
        const std::vector<Slice> &slices = timing ? timed : plain;
        for (const Slice &s : slices) {
            std::vector<std::pair<CellInfo *, int>> cells;
            for (int side = 0; side < 2; side++) {
                if (s.lut[side])
                    cells.emplace_back(s.lut[side], Z_FLUT + side);
                if (s.ff[side])
                    cells.emplace_back(s.ff[side], Z_FFX + side);
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
        }
        log_info("Packed %d LUTs and flip-flops into %d slices (%s).\n", int(unit_of.size()), int(slices.size()),
                 timing ? "timing-driven" : "by shared nets");
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

    // ------------------------------------------------------------------ routing
    // A global buffer's net reaches its clock pins on the global lines, routed before everything else. (On general
    // routing it would be cheaper for the router, and skewed: trce's 1.5 ns on a clock pin, ISE's DRC silent.)
    bool on_general_routing(WireId w) const
    {
        static const std::regex general("([EWNS](2|6).*|OMUX.*|L[HV].*|[XY]Q?\\d*)");
        return std::regex_match(ctx->getWireName(w)[1].str(ctx), general);
    }

    // BX of a chain's first stage brings its carry in: its DI cannot come through BX too (a net of its own: pin 1 of the
    // LUT). Router 1 would learn that by ripping up, for minutes on a design of a few hundred slices.
    pool<PipId> blocked_pips;
    bool checkPipAvail(PipId pip) const override { return blocked_pips.empty() || !blocked_pips.count(pip); }

    void block_di_on_carry_in()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *m = cell.second.get();
            if (m->type != id_MUXCY || m->bel == BelId() || ctx->getBelLocation(m->bel).z % Z_STRIDE != Z_CYMUXF)
                continue;
            NetInfo *ci = m->getPort(id_CI), *di = m->getPort(id_DI);
            if (!ci || !di || ci == di || (ci->driver.cell && ci->driver.cell->type == id_MUXCY))
                continue;
            WireId w = ctx->getBelPinWire(m->bel, id_DI);
            WireId bx;
            for (PipId up : ctx->getPipsUphill(ctx->getBelPinWire(m->bel, id_CI)))
                if (ctx->getWireName(ctx->getPipSrcWire(up))[1].str(ctx).rfind("CIN", 0) != 0)
                    bx = ctx->getPipSrcWire(up);
            for (PipId up : ctx->getPipsUphill(w))
                if (ctx->getPipSrcWire(up) == bx)
                    blocked_pips.insert(up);
        }
    }

    void preRoute() override
    {
        block_di_on_carry_in();
        std::vector<NetInfo *> clocks;
        for (auto &net : ctx->nets)
            if (on_global(net.second.get()))
                clocks.push_back(net.second.get());
        for (NetInfo *net : clocks) {
            WireId source = ctx->getNetinfoSourceWire(net);
            ctx->bindWire(source, net, STRENGTH_LOCKED);
            int left = 0;
            for (auto &usr : net->users) {
                if (!has_prefix(usr.port.str(ctx), "CLK"))
                    continue;
                std::queue<WireId> visit;
                dict<WireId, PipId> back;
                WireId sink = ctx->getNetinfoSinkWire(net, usr, 0), found;
                visit.push(sink);
                while (!visit.empty() && found == WireId()) {
                    WireId w = visit.front();
                    visit.pop();
                    if (ctx->getBoundWireNet(w) == net) {
                        found = w;
                        break;
                    }
                    for (PipId up : ctx->getPipsUphill(w)) {
                        WireId src = ctx->getPipSrcWire(up);
                        if (back.count(src) || !ctx->checkPipAvail(up) || on_general_routing(src))
                            continue;
                        if (!ctx->checkWireAvail(src) && ctx->getBoundWireNet(src) != net)
                            continue;
                        back[src] = up;
                        visit.push(src);
                    }
                }
                if (found == WireId()) {
                    left++;   // (left to the router)
                    continue;
                }
                // the route found, from the net's wires down to the pin: each wire with the PIP into it
                for (WireId w = found; w != sink;) {
                    PipId down = back.at(w);
                    w = ctx->getPipDstWire(down);
                    if (ctx->getBoundWireNet(w) != net)
                        ctx->bindPip(down, net, STRENGTH_LOCKED);
                }
            }
            if (left)
                log_warning("the clock '%s': %d clock pin(s) out of the reach of its global line.\n", ctx->nameOf(net), left);
        }
    }

    // ------------------------------------------------------------------ validity
    // A slice holding a carry chain's stage or a wide multiplexer takes no cell of another cluster: their BX / BY (a
    // chain's carry in or DI, a multiplexer's select) would be another cell's too (a flip-flop's D through BX), and the
    // router could not part them.
    // a slice whose GND source is used and that holds other cells
    bool gnd_shares_slice(BelId bel) const
    {
        Loc l = ctx->getBelLocation(bel);
        int base = l.z - l.z % Z_STRIDE;
        BelId g = ctx->getBelByLocation(Loc(l.x, l.y, base + Z_GND));
        if (g == BelId() || !ctx->getBoundBelCell(g))
            return false;
        for (int z = 0; z <= Z_FXMUX; z++) {
            BelId b = ctx->getBelByLocation(Loc(l.x, l.y, base + z));
            if (b != BelId() && ctx->getBoundBelCell(b))
                return true;
        }
        return false;
    }

    bool foreign_in_slice(BelId bel) const
    {
        Loc l = ctx->getBelLocation(bel);
        int base = l.z - l.z % Z_STRIDE;
        auto cell_at = [&](int z) -> const CellInfo * {
            BelId b = ctx->getBelByLocation(Loc(l.x, l.y, base + z));
            return b == BelId() ? nullptr : ctx->getBoundBelCell(b);
        };
        const CellInfo *owner = nullptr;
        for (int z : {Z_CYMUXF, Z_CYMUXG, Z_XORF, Z_XORG, Z_F5MUX, Z_FXMUX, Z_FMEM, Z_GMEM})
            if (!owner)
                owner = cell_at(z);
        if (!owner)
            return false;
        for (int z = 0; z <= Z_GMEM; z++)
            if (const CellInfo *k = cell_at(z))
                if (k->cluster != owner->cluster)
                    return true;
        return false;
    }

    // The two flip-flops of a slice share CLK, CE, SR and the set / reset mode.
    bool isBelLocationValid(BelId bel, bool explain_invalid) const override
    {
        Loc l = ctx->getBelLocation(bel);
        int off = l.z % Z_STRIDE;
        if (off <= Z_GMEM && ctx->getBoundBelCell(bel) && foreign_in_slice(bel)) {
            if (explain_invalid)
                log_nonfatal_error("%s: in a slice of a carry chain, a wide multiplexer or a memory it is not part of\n",
                                   ctx->nameOf(ctx->getBoundBelCell(bel)));
            return false;
        }
        if ((off <= Z_FXMUX || off == Z_GND) && ctx->getBoundBelCell(bel) && gnd_shares_slice(bel)) {
            if (explain_invalid)
                log_nonfatal_error("%s: a constant 0's slice has room for nothing else\n", ctx->nameOf(ctx->getBoundBelCell(bel)));
            return false;
        }
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
