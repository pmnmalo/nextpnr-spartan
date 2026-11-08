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

// Spartan-6: the pads and the global clock buffers (Silinx #3136), as Silinx's placer had them (its fpga/s6/place.js
// placeS6, sites.js): the bonded pads, their banks (one per side of the CSG324 parts, UG381: 0 top, 1 right, 2 bottom,
// 3 left) and the global buffers a pad's dedicated clock path reaches (a GCLK pin).

#ifndef SPARTAN6_PADS_H
#define SPARTAN6_PADS_H

#include <string>
#include <vector>

#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

namespace s6 {

struct Pads
{
    std::vector<BelId> pads;           // the bonded pads, in the device's order
    dict<BelId, int> bank;
    dict<BelId, std::vector<BelId>> bufgs;   // the BUFGMUXes a pad's dedicated path reaches, nearest first
    std::vector<BelId> bufg_order;     // the BUFGMUXes, in the device's order
    void init(Context *ctx);
    bool bonded(BelId b) const { return bank.count(b) > 0; }
};

// the VCCO an I/O standard needs (in tenths of a volt; 0: unknown, no constraint)
int vcco_of(const std::string &std);
// an IOB cell's I/O standard (LVCMOS25, ISE's default, when it has none)
std::string iostandard(Context *ctx, const CellInfo *iob);

// The pads of the IOBs and the global buffers, fixed (their attribute BEL): a LOC's pad, a global buffer driven by a
// pad on the buffer its dedicated path reaches (a clock input without a LOC on a free pad whose buffer is free), the
// others on the free buffers in order. The IOBs without a LOC are the placer's.
void place_pads(Context *ctx, const Pads &pads);

} // namespace s6

NEXTPNR_NAMESPACE_END

#endif
