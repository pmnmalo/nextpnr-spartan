/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Claire Xenia Wolf <claire@yosyshq.com>
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

#ifndef ROUTER1_H
#define ROUTER1_H

#include "log.h"
#include "nextpnr.h"
NEXTPNR_NAMESPACE_BEGIN

struct Router1Cfg
{
    Router1Cfg(Context *ctx);

    int maxIterCnt;
    bool cleanupReroute;
    bool fullCleanupReroute;
    bool useEstimate;
    delay_t wireRipupPenalty;
    delay_t netRipupPenalty;
    delay_t reuseBonus;
    delay_t estimatePrecision;
    // router1/maxVisitsPerArc and router1/maxVisits: the effort, as the wires the searches may visit in all, per arc
    // of the design and at most (0: no limit). Past it router 1 stops with the routes it has, unfinished, and sets
    // router1/gaveUp: a congested design whose searches flood the device as they rip up, or a large one that
    // converges slowly, could take hours (pmnmalo/Silinx-ISE#2912).
    int64_t maxVisitsPerArc;
    int64_t maxVisits;
};

extern bool router1(Context *ctx, const Router1Cfg &cfg);

NEXTPNR_NAMESPACE_END

#endif // ROUTER1_H
