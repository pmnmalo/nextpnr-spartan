/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019-2023  Myrtle Shah <gatecat@ds0.me>
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

#include <algorithm>
#include <boost/optional.hpp>
#include <iterator>
#include <queue>
#include <unordered_set>
#include "chain_utils.h"
#include "design_utils.h"
#include "extra_data.h"
#include "log.h"
#include "nextpnr.h"
#include "pack.h"
#include "pins.h"

#define HIMBAECHEL_CONSTIDS "uarch/xilinx/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

bool XC7Packer::has_illegal_fanout(NetInfo *carry)
{
    // FIXME: sometimes we can feed out of the chain
    if (carry->users.entries() > 2)
        return true;
    CellInfo *muxcy = nullptr, *xorcy = nullptr;
    for (auto &usr : carry->users) {
        if (usr.cell->type == id_MUXCY) {
            if (muxcy != nullptr)
                return true;
            else if (usr.port != id_CI)
                return true;
            else
                muxcy = usr.cell;
        } else if (usr.cell->type == id_XORCY) {
            if (xorcy != nullptr)
                return true;
            else if (usr.port != id_CI)
                return true;
            else
                xorcy = usr.cell;
        } else {
            return true;
        }
    }
    if (muxcy && xorcy) {
        NetInfo *muxcy_s = muxcy->getPort(id_S);
        NetInfo *xorcy_li = xorcy->getPort(id_LI);
        if (muxcy_s != xorcy_li)
            return true;
    }
    return false;
}

void XilinxPacker::split_carry4s()
{
    std::vector<CellInfo *> carry4s;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type != id_CARRY4)
            continue;
        carry4s.push_back(ci);
    }
    for (CellInfo *ci : carry4s) {
        NetInfo *cin = ci->getPort(id_CI);
        if (cin == nullptr || cin->name == ctx->id("$PACKER_GND_NET")) {
            cin = ci->getPort(id_CYINIT);
        }
        ci->disconnectPort(id_CI);
        ci->disconnectPort(id_CYINIT);

        for (int i = 0; i < 4; i++) {
            // A bit left unconnected (unused, see pack_carries) has no XORCY
            NetInfo *o = ci->getPort(ctx->idf("O[%d]", i));
            bool unused = ci->getPort(ctx->idf("S[%d]", i)) == nullptr && (o == nullptr || o->users.empty());
            CellInfo *xorcy =
                    unused ? nullptr : create_cell(id_XORCY, ctx->idf("%s$split$xorcy%d", ci->name.c_str(ctx), i));
            CellInfo *muxcy = create_cell(id_MUXCY, ctx->idf("%s$split$muxcy%d", ci->name.c_str(ctx), i));
            muxcy->connectPort(id_CI, cin);
            ci->movePortTo(ctx->idf("DI[%d]", i), muxcy, id_DI);
            muxcy->connectPort(id_S, ci->getPort(ctx->id("S[" + std::to_string(i) + "]")));
            if (xorcy) {
                xorcy->connectPort(id_CI, cin);
                ci->movePortTo(ctx->idf("S[%d]", i), xorcy, id_LI);
                ci->movePortTo(ctx->idf("O[%d]", i), xorcy, id_O);
            } else {
                ci->disconnectPort(ctx->idf("O[%d]", i));
            }
            NetInfo *co = ci->getPort(ctx->idf("CO[%d]", i));
            ci->disconnectPort(ctx->idf("CO[%d]", i));
            if (!co)
                co = create_internal_net(ci->name, stringf("$split$co%d", i), false);
            muxcy->connectPort(id_O, co);
            cin = co;
        }
        packed_cells.insert(ci->name);
    }
    flush_cells();
}

void XC7Packer::pack_carries()
{
    log_info("Packing carries..\n");
    // A CARRY4's top bits whose O and CO nothing reads (an adder's padding up to the CARRY4's width) are don't
    // cares: their S and DI need no LUT, whatever drives them (Yosys ties them to constants)
    int dead_bits = 0;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type != id_CARRY4)
            continue;
        for (int z = 3; z >= 0; z--) {
            NetInfo *o = ci->getPort(ctx->idf("O[%d]", z)), *co = ci->getPort(ctx->idf("CO[%d]", z));
            if ((o && !o->users.empty()) || (co && !co->users.empty()))
                break;
            for (auto p : {ctx->idf("S[%d]", z), ctx->idf("DI[%d]", z)}) {
                if (ci->getPort(p))
                    ci->disconnectPort(p);
            }
            dead_bits++;
        }
    }
    if (dead_bits > 0)
        log_info("    Left the S and DI of %d unused CARRY4 bits unconnected\n", dead_bits);
    split_carry4s();
    std::vector<CellInfo *> root_muxcys;
    // Find MUXCYs
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type != id_MUXCY)
            continue;
        NetInfo *ci_net = ci->getPort(id_CI);
        if (!ci_net || !ci_net->driver.cell || ci_net->driver.cell->type != id_MUXCY || has_illegal_fanout(ci_net)) {
            root_muxcys.push_back(ci);
        }
    }

    unsigned muxcy_feed_through_luts = 0;

    // Create chains from root MUXCYs
    pool<IdString> processed_muxcys;
    std::vector<CarryGroup> groups;
    int muxcy_count = 0, xorcy_count = 0;
    for (auto root : root_muxcys) {
        CarryGroup group;

        CellInfo *muxcy = root;
        NetInfo *mux_ci = nullptr;
        while (true) {

            group.muxcys.push_back(muxcy);
            ++muxcy_count;
            mux_ci = muxcy->getPort(id_CI);
            NetInfo *mux_s = muxcy->getPort(id_S);
            group.xorcys.push_back(nullptr);
            if (mux_s != nullptr) {
                for (auto &user : mux_s->users) {
                    if (user.cell->type == id_XORCY && user.port == id_LI) {
                        CellInfo *xorcy = user.cell;
                        NetInfo *xor_ci = xorcy->getPort(id_CI);
                        if (xor_ci == mux_ci) {
                            group.xorcys.back() = xorcy;
                            ++xorcy_count;
                            break;
                        }
                    }
                }
            }

            mux_ci = muxcy->getPort(id_O);
            if (mux_ci == nullptr)
                break;
            if (has_illegal_fanout(mux_ci))
                break;
            muxcy = nullptr;
            for (auto &user : mux_ci->users) {
                if (user.cell->type == id_MUXCY) {
                    muxcy = user.cell;
                    break;
                }
            }
            if (muxcy == nullptr)
                break;
        }
        if (mux_ci != nullptr) {
            if (mux_ci->users.entries() == 1 && (*mux_ci->users.begin()).cell->type == id_XORCY &&
                (*mux_ci->users.begin()).port == id_CI) {
                // Trailing XORCY at end, can pack into chain.
                CellInfo *xorcy = (*mux_ci->users.begin()).cell;
                CellInfo *dummy_muxcy = create_cell(id_MUXCY, ctx->idf("%s$legal_muxcy$", xorcy->name.c_str(ctx)));
                dummy_muxcy->connectPort(id_CI, mux_ci);
                dummy_muxcy->connectPort(id_S, xorcy->getPort(id_LI));
                group.muxcys.push_back(dummy_muxcy);
                group.xorcys.push_back(xorcy);
            } else if (mux_ci->users.entries() > 0) {
                // Users other than a MUXCY
                // Feed out with a zero-driving LUT and a XORCY
                // (creating a zero-driver using Vcc and an inverter for now...)
                CellInfo *zero_lut = create_lut(mux_ci->name.str(ctx) + "$feed$zero",
                                                {ctx->nets.at(ctx->id("$PACKER_VCC_NET")).get()}, nullptr, Property(1));
                muxcy_feed_through_luts++;
                CellInfo *feed_xorcy = create_cell(id_XORCY, ctx->id(mux_ci->name.str(ctx) + "$feed$xor"));
                CellInfo *dummy_muxcy = create_cell(id_MUXCY, ctx->id(mux_ci->name.str(ctx) + "$feed$muxcy"));

                CellInfo *last_muxcy = mux_ci->driver.cell;

                last_muxcy->disconnectPort(id_O);

                zero_lut->connectPorts(id_O, feed_xorcy, id_LI);
                zero_lut->connectPorts(id_O, dummy_muxcy, id_S);
                last_muxcy->connectPorts(id_O, feed_xorcy, id_CI);
                last_muxcy->connectPorts(id_O, dummy_muxcy, id_CI);

                feed_xorcy->connectPort(id_O, mux_ci);

                group.muxcys.push_back(dummy_muxcy);
                group.xorcys.push_back(feed_xorcy);
            }
        }

        groups.push_back(group);
    }
    flush_cells();

    if (muxcy_feed_through_luts > 0)
        log_info("    Created %d feed-through LUTs from MUXCY entries\n", muxcy_feed_through_luts);
    log_info("    Grouped %d MUXCYs and %d XORCYs into %d chains\n", muxcy_count, xorcy_count, int(root_muxcys.size()));

    // A DI LUT is in the 5LUT (at most five inputs); an S LUT may be a LUT6 when the 5LUT is not needed
    pool<IdString> lut_types{id_LUT1, id_LUT2, id_LUT3, id_LUT4, id_LUT5};
    pool<IdString> lut6_types{id_LUT1, id_LUT2, id_LUT3, id_LUT4, id_LUT5, id_LUT6};

    pool<IdString> folded_nets;

    unsigned carry_feed_through_luts = 0;
    int feed_s_ff = 0, feed_s_const = 0, feed_s_lut = 0, feed_s_other = 0, feed_di = 0;

    for (auto &grp : groups) {
        std::vector<CellInfo *> carry4s;
        for (int i = 0; i < int(grp.muxcys.size()); i++) {
            int z = i % 4;
            CellInfo *muxcy = grp.muxcys.at(i), *xorcy = grp.xorcys.at(i);
            if (z == 0)
                carry4s.push_back(create_cell(id_CARRY4, ctx->idf("%s$PACKED_CARRY4$", muxcy->name.c_str(ctx))));
            CellInfo *c4 = carry4s.back();
            CellInfo *root = carry4s.front();
            if (i == 0) {
                // Constrain initial CARRY4, forcing it to the CARRY4 of a logic tile
                c4->cluster = c4->name;
                c4->constr_abs_z = true;
                c4->constr_z = BEL_CARRY4;
            } else if (z == 0) {
                // Constrain relative to the root carry4
                c4->cluster = root->name;
                root->constr_children.push_back(c4);
                c4->constr_x = 0;
                // Looks no CARRY4 on the tile of which grid_y is a multiple of 26. Skip them
                c4->constr_y = -(i / 4 + i / (4 * 25));
                c4->constr_abs_z = true;
                c4->constr_z = BEL_CARRY4;
            }
            // Fold CI->CO connections into the CARRY4, except for those external ones every 8 units
            if (z == 0 && i == 0) {
                muxcy->movePortTo(id_CI, c4, id_CYINIT);
                // A constant carry-in is PRECYINIT's 0 or 1, a configuration bit: no route, and the bypass stays free
                NetInfo *cyinit = c4->getPort(id_CYINIT);
                if (cyinit && cyinit->driver.cell && cyinit->driver.cell->type.in(id_PSEUDO_GND, id_PSEUDO_VCC)) {
                    c4->attrs[ctx->id("X_PRECYINIT")] = Property(std::string((cyinit->driver.cell->type == id_PSEUDO_VCC) ? "C1" : "C0"));
                    c4->disconnectPort(id_CYINIT);
                }
            } else if (z == 0 && i > 0) {
                muxcy->movePortTo(id_CI, c4, id_CI);
            } else {
                NetInfo *muxcy_ci = muxcy->getPort(id_CI);
                if (muxcy_ci)
                    folded_nets.insert(muxcy_ci->name);
                muxcy->disconnectPort(id_CI);
            }
            if (z == 3) {
                muxcy->movePortTo(id_O, c4, ctx->id("CO[3]"));
            } else {
                NetInfo *muxcy_o = muxcy->getPort(id_O);
                if (muxcy_o)
                    folded_nets.insert(muxcy_o->name);
                muxcy->disconnectPort(id_O);
            }
            // Replace connections into the MUXCY with external CARRY4 ports
            muxcy->movePortTo(id_S, c4, ctx->idf("S[%d]", z));
            muxcy->movePortTo(id_DI, c4, ctx->idf("DI[%d]", z));
            packed_cells.insert(muxcy->name);
            // Fold MUXCY->XORCY into the CARRY4, if there is a XORCY
            if (xorcy) {
                // Replace XORCY output with external CARRY4 output
                xorcy->movePortTo(id_O, c4, ctx->idf("O[%d]", z));
                // Disconnect internal XORCY connectivity
                xorcy->disconnectPort(id_LI);
                xorcy->disconnectPort(id_DI);
                packed_cells.insert(xorcy->name);
            }
            // Make the LUTs driving the CARRY4 legal, as Vivado packs a carry chain: S comes from its own slice's
            // O6, DI from O5 or from the slice's AX..DX bypass (the CY0 mux)
            NetInfo *c4_s = c4->getPort(ctx->idf("S[%d]", z));
            NetInfo *c4_di = c4->getPort(ctx->idf("DI[%d]", z));
            auto lut_inputs = [&](CellInfo *lut) {
                std::vector<NetInfo *> ins;
                for (int j = 0; j < 6; j++) {
                    NetInfo *ix = lut->getPort(ctx->idf("I%d", j));
                    if (ix && std::find(ins.begin(), ins.end(), ix) == ins.end())
                        ins.push_back(ix);
                }
                return ins;
            };
            auto free_lut = [&](NetInfo *n, const pool<IdString> &types) {
                return n && n->driver.cell != nullptr && types.count(n->driver.cell->type) &&
                       n->driver.port == id_O && !is_constrained(n->driver.cell) &&
                       !packed_cells.count(n->driver.cell->name);
            };
            // S: its LUT in the 6LUT, even when that LUT drives logic elsewhere too (O6 leaves the slice by the A..D
            // output as well); a LUT6 only when DI does not need the 5LUT
            CellInfo *s_lut = nullptr, *di_lut = nullptr;
            bool s_feed = false, di_bypass = false;
            if (free_lut(c4_s, lut6_types))
                s_lut = c4_s->driver.cell;
            std::vector<NetInfo *> s_ins;
            if (s_lut)
                s_ins = lut_inputs(s_lut);
            // The bypass of bit 0 is the chain's CYINIT when that is a signal of its own
            NetInfo *cyinit = (z == 0) ? c4->getPort(id_CYINIT) : nullptr;
            bool ax_free = cyinit == nullptr || cyinit == c4_di;
            if (c4_di) {
                // DI: a LUT of its own in the 5LUT when it fits beside S's inputs (at most five in all)
                if (free_lut(c4_di, lut_types) && c4_di->users.entries() == 1 && int(s_ins.size()) <= 5) {
                    std::vector<NetInfo *> all = s_ins;
                    for (auto ix : lut_inputs(c4_di->driver.cell))
                        if (std::find(all.begin(), all.end(), ix) == all.end())
                            all.push_back(ix);
                    if (int(all.size()) + (s_lut ? 0 : 1) <= 5)
                        di_lut = c4_di->driver.cell;
                }
                // Otherwise the bypass: a flip-flop, a port, a constant (GND by the INT tile's GFAN, VCC the
                // bypass's default), a LUT driving other logic too
                if (!di_lut && ax_free)
                    di_bypass = true;
            }
            if (s_lut && int(s_ins.size()) == 6 && di_lut == nullptr && !di_bypass)
                s_lut = nullptr; // DI needs the 5LUT (bit 0 with CYINIT on the bypass)
            if (!s_lut && c4_s) {
                // A feed-through (Vivado's route-thru) only where the site needs one: S from a flip-flop, a port, a
                // constant, or a LUT packed elsewhere
                PortRef pr;
                pr.cell = c4;
                pr.port = ctx->idf("S[%d]", z);
                CellInfo *drv = c4_s->driver.cell;
                if (drv == nullptr || drv->type.in(id_PSEUDO_GND, id_PSEUDO_VCC))
                    feed_s_const++;
                else if (drv->type.in(id_FDRE, id_FDSE, id_FDCE, id_FDPE))
                    feed_s_ff++;
                else if (lut6_types.count(drv->type))
                    feed_s_lut++;
                else
                    feed_s_other++;
                s_lut = feed_through_lut(c4_s, {pr});
                s_feed = true;
                carry_feed_through_luts++;
            }
            if (c4_di && !di_lut && !di_bypass) {
                // DI beside a CYINIT on the bypass: O5, a feed-through sharing the S LUT's inputs if it can
                int s_count = int(lut_inputs(s_lut).size());
                bool shared = std::find(s_ins.begin(), s_ins.end(), c4_di) != s_ins.end();
                if (s_count + (shared ? 0 : 1) > 5 && !s_feed) {
                    // No room: S moves out to a feed-through as well
                    PortRef pr;
                    pr.cell = c4;
                    pr.port = ctx->idf("S[%d]", z);
                    s_lut = feed_through_lut(c4_s, {pr});
                    feed_s_lut++;
                    carry_feed_through_luts++;
                }
                PortRef pr;
                pr.cell = c4;
                pr.port = ctx->idf("DI[%d]", z);
                di_lut = feed_through_lut(c4_di, {pr});
                feed_di++;
                carry_feed_through_luts++;
            }
            // Constrain LUTs relative to root CARRY4
            if (s_lut) {
                root->constr_children.push_back(s_lut);
                s_lut->cluster = root->name;
                s_lut->constr_x = 0;
                s_lut->constr_y = -(i / 4 + i / (4 * 25));
                s_lut->constr_abs_z = true;
                s_lut->constr_z = (z << 4 | BEL_6LUT);
            }
            if (di_lut) {
                root->constr_children.push_back(di_lut);
                di_lut->cluster = root->name;
                di_lut->constr_x = 0;
                di_lut->constr_y = -(i / 4 + i / (4 * 25));
                di_lut->constr_abs_z = true;
                di_lut->constr_z = (z << 4 | BEL_5LUT);
            }
        }
    }
    flush_cells();

    for (auto net : folded_nets)
        ctx->nets.erase(net);

    // XORCYs and MUXCYs not part of any chain (and therefore not packed into a CARRY4) must now be blasted
    // to boring soft logic (LUT2 or LUT3 - these will become SLICE_LUTXs later in the flow.)
    int remaining_muxcy = 0, remaining_xorcy = 0;
    for (auto &cell : ctx->cells) {
        if (cell.second->type == id_MUXCY)
            ++remaining_muxcy;
        else if (cell.second->type == id_XORCY)
            ++remaining_xorcy;
    }
    dict<IdString, XFormRule> softlogic_rules;
    softlogic_rules[id_MUXCY].new_type = id_LUT3;
    softlogic_rules[id_MUXCY].port_xform[id_DI] = id_I0;
    softlogic_rules[id_MUXCY].port_xform[id_CI] = id_I1;
    softlogic_rules[id_MUXCY].port_xform[id_S] = id_I2;
    // DI 1010 1010
    // CI 1100 1100
    //  S 1111 0000
    //  O 1100 1010
    softlogic_rules[id_MUXCY].set_params.emplace_back(id_INIT, Property(0xCA));

    softlogic_rules[id_XORCY].new_type = id_LUT2;
    softlogic_rules[id_XORCY].port_xform[id_CI] = id_I0;
    softlogic_rules[id_XORCY].port_xform[id_LI] = id_I1;
    // CI 1100
    // LI 1010
    //  O 0110
    softlogic_rules[id_XORCY].set_params.emplace_back(id_INIT, Property(0x6));

    generic_xform(softlogic_rules, false);
    if (carry_feed_through_luts > 0)
        log_info("    Created %d feed-through LUTs from carry chains (S from a flip-flop %d, a constant %d, a LUT packed "
                 "elsewhere %d, other %d; DI %d)\n",
                 carry_feed_through_luts, feed_s_ff, feed_s_const, feed_s_lut, feed_s_other, feed_di);
    log_info("    Blasted %d non-chain MUXCYs and %d non-chain XORCYs to soft logic\n", remaining_muxcy,
             remaining_xorcy);

    // Finally, use generic_xform to remove the [] from bus ports; and set up the logical-physical mapping for
    // RapidWright
    dict<IdString, XFormRule> c4_rules;
    c4_rules[id_CARRY4].new_type = id_CARRY4;
    c4_rules[id_CARRY4].port_xform[id_CI] = id_CIN;

    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type != id_CARRY4)
            continue;
        xform_cell(c4_rules, ci);
    }
}

NEXTPNR_NAMESPACE_END