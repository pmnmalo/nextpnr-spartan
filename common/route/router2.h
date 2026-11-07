/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019  gatecat <gatecat@ds0.me>
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

#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

inline float default_base_cost(Context *ctx, WireId wire, PipId pip, float crit_weight)
{
    (void)crit_weight; // unused
    return ctx->getDelayNS(ctx->getPipDelay(pip).maxDelay() + ctx->getWireDelay(wire).maxDelay() +
                           ctx->getDelayEpsilon());
}

struct Router2Cfg
{
    Router2Cfg(Context *ctx);

    // Maximum iterations for backwards routing attempt
    int backwards_max_iter;
    // Maximum iterations for backwards routing attempt for global nets
    int global_backwards_max_iter;
    // Padding added to bounding boxes to account for imperfect routing,
    // congestion, etc
    int bb_margin_x, bb_margin_y;
    // Cost factor added to input pin wires; effectively reduces the
    // benefit of sharing interconnect
    float ipin_cost_adder;
    // Cost factor for "bias" towards center location of net
    float bias_cost_factor;
    // Starting current and historical congestion cost factor
    float init_curr_cong_weight, hist_cong_weight;
    // Current congestion cost multiplier
    float curr_cong_mult;

    // Weight given to delay estimate in A*. Higher values
    // mean faster and more directed routing, at the risk
    // of choosing a less congestion/delay-optimal route
    float estimate_weight;

    // The iterations at most, past which routing fails with an error naming the overused wires (router2/maxIter; 0,
    // the default: no limit, router 2 iterating until it converges)
    int max_iter = 0;

    // A stall: when the overuse has not gone down for stall_iter iterations, the present congestion weight grows by
    // stall_cong_mult more each iteration, until it does (router2/stallIter, router2/stallCongMult; 0, the default: no
    // such growth). With the alternate weights the weight does not grow at all, and a congested wire costs a critical
    // arc little more than a free one (its crit_weight down to 0.05): two critical nets may share one wire for ever.
    int stall_iter = 0;
    float stall_cong_mult = 0;
    // and from that iteration on, stalled or not (router2/growIter; 0, the default: never): a large congested design's
    // overuse goes down slowly with the alternate weights, and needs some 500 iterations (Silinx's epfl-multiplier)
    int grow_iter = 0;

    // Print additional performance profiling information
    bool perf_profile = false;

    std::string heatmap;
    std::function<float(Context *ctx, WireId wire, PipId pip, float crit_weight)> get_base_cost = default_base_cost;
};

void router2(Context *ctx, const Router2Cfg &cfg);

NEXTPNR_NAMESPACE_END
