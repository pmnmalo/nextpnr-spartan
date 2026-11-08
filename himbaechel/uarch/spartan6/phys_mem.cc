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

// Spartan-6: the site pins of the SLICEM's distributed RAMs and shift registers, as ISE's map places them (Silinx's
// fpga/s6/phys-mem.js, #1153). A cell takes one or more LUT positions of its slice (D first); the write address is the
// D LUT's inputs:
//   RAM32X1S / D        a 32-bit RAM in the 64-bit mode: A1 low, the address on A2 … A6
//   RAM64X1S / D        the address on A1 … A6; RAM128X1S / D, RAM256X1S: two or four of them, the 7th address bit
//                       the F7's select (AX, CX; CX also WA7), the 8th the F8's (BX, WA8)
//   RAM32M / RAM64M     each position its own address (ADDRA on A1 …, A6 high for the 32-bit O6 / O5 pair)
//   SRL16 … / SRLC32E   the address on A2 … A5 (A6), A1 high (A6 too for a 16-bit one)
// The data: DX in a RAM's D, AX … CX in a position of its own (a RAM32M's DIA<1> … on AI … DI); a shift register's on
// AI … DI (16-bit) or AX … DX (32-bit). The outputs: O6 on A … D, a RAM32M's O5 on AMUX …, an F7 on AMUX / CMUX, the F8
// on BMUX, a shift register's cascade on DMUX from position A only.

#include <regex>

#include "pack.h"

NEXTPNR_NAMESPACE_BEGIN

namespace s6 {

namespace {

// a pin's bit: A<3> / A3 -> (A, 3); D -> (D, -1)
std::pair<std::string, int> bit_of(const std::string &pin)
{
    static const std::regex re("([A-Z]+?)(?:<([0-9]+)>|([0-9]+))?");
    std::smatch m;
    if (!std::regex_match(pin, m, re))
        return {pin, -1};
    if (m[2].matched)
        return {m[1], std::stoi(m[2])};
    if (m[3].matched)
        return {m[1], std::stoi(m[3])};
    return {m[1], -1};
}

struct Part
{
    std::string p;
    char port;   // 'w' the write port's address, 'r' a read port's
    int half;
};

std::vector<Part> mem_layout(const NCell &c, const std::vector<std::string> &ps)
{
    const std::string &t = c.type;
    std::vector<Part> parts;
    if (is_srl(t))
        return {{ps.at(0), 'w', 0}};
    if (t == "RAM32M" || t == "RAM64M") {
        for (auto &p : ps)
            parts.push_back({p, 'w', 0});
        return parts;
    }
    bool dual = t.size() >= 3 && t.compare(t.size() - 3, 3, "X1D") == 0;
    int per = mem_depth(t) > 64 ? mem_depth(t) / 64 : 1;
    for (int i = 0; i < int(ps.size()); i++)
        parts.push_back({ps[i], dual && i >= per ? 'r' : 'w', i % per});
    return parts;
}

std::string f7_select(const std::string &p) { return p == "A" || p == "B" ? "AX" : "CX"; }

std::string data_pin(const std::vector<Part> &parts)
{
    std::vector<std::string> w;
    for (auto &q : parts)
        if (q.port == 'w')
            w.push_back(q.p);
    return std::find(w.begin(), w.end(), "D") != w.end() ? "DX" : w.at(0) + "X";
}

} // namespace

int mem_depth(const std::string &type)
{
    static const std::regex re("RAM([0-9]+).*");
    std::smatch m;
    return std::regex_match(type, m, re) ? std::stoi(m[1]) : 0;
}

std::vector<std::string> mem_positions(const Slice &s, int ci)
{
    std::vector<std::string> ps;
    for (int p = 3; p >= 0; p--)
        if (s.pos[p].o6.cell() && s.pos[p].o6.v == ci)
            ps.push_back(std::string(1, POS[p]));
    return ps;
}

std::vector<std::string> mem_in_pins(const NCell &c, const std::vector<std::string> &ps, const std::string &pin, int k)
{
    const std::string &t = c.type;
    auto [name, bit] = bit_of(pin);
    if (is_srl(t)) {
        const std::string &p = ps.at(0);
        if (name == "D")
            return {t == "SRLC32E" ? p + "X" : p + "I"};
        if (name == "CLK")
            return k < 0 ? std::vector<std::string>{"CLK"} : std::vector<std::string>{};
        if (name == "CE")
            return k == 1 ? std::vector<std::string>{} : std::vector<std::string>{"CE"};
        if (name == "A")
            return {p + std::to_string(bit + 2)};
        return {};
    }
    if (name == "WCLK")
        return k < 0 ? std::vector<std::string>{"CLK"} : std::vector<std::string>{};
    if (name == "WE")
        return k == 1 ? std::vector<std::string>{} : std::vector<std::string>{"CE"};
    auto parts = mem_layout(c, ps);
    if (t == "RAM32M" || t == "RAM64M") {
        static const std::regex re("(ADDR|DI)([A-D])");
        std::smatch m;
        if (!std::regex_match(name, m, re))
            return {};
        if (m[1] == "ADDR")
            return {m[2].str() + std::to_string(bit + 1)};
        return {m[2].str() + (t == "RAM32M" && bit == 1 ? "I" : "X")};
    }
    int shift = mem_depth(t) == 32 ? 2 : 1;
    if (name == "D")
        return {data_pin(parts)};
    char port = name == "A" ? 'w' : name == "DPRA" ? 'r' : 0;
    if (!port || bit < 0)
        return {};
    std::vector<Part> mine;
    for (auto &q : parts)
        if (q.port == port)
            mine.push_back(q);
    std::vector<std::string> out;
    if (bit < 6) {
        for (auto &q : mine)
            out.push_back(q.p + std::to_string(bit + shift));
        return out;
    }
    // the 7th bit: the F7s' select of the port's pairs (and WA7, CX, for the write port); the 8th: the F8's (BX)
    if (bit == 6) {
        for (auto &q : mine)
            if (std::find(out.begin(), out.end(), f7_select(q.p)) == out.end())
                out.push_back(f7_select(q.p));
        if (port == 'w' && std::find(out.begin(), out.end(), "CX") == out.end())
            out.push_back("CX");
        return out;
    }
    return bit == 7 ? std::vector<std::string>{"BX"} : std::vector<std::string>{};
}

std::vector<std::pair<std::string, int>> mem_const_pins(const NCell &c, const std::vector<std::string> &ps,
                                                       const std::function<int(int)> &konst)
{
    const std::string &t = c.type;
    std::vector<std::pair<std::string, int>> out;
    if (is_srl(t)) {
        out.push_back({ps.at(0) + "1", 1});
        if (t != "SRLC32E")
            out.push_back({ps.at(0) + "6", 1});
        // (SRL16 has no CE: tied high, as ISE ties it)
        int ce = konst(c.pin("CE", 1));
        if (ce >= 0)
            out.push_back({"CE", ce});
        for (auto &pn : c.pins) {
            auto [name, bit] = bit_of(pn.first);
            if (name == "A" && konst(pn.second) >= 0)
                out.push_back({ps.at(0) + std::to_string(bit + 2), konst(pn.second)});
        }
        return out;
    }
    int we = konst(c.pin("WE", 1));
    if (we >= 0)
        out.push_back({"CE", we});
    if (t == "RAM32M")
        for (auto &p : ps)
            out.push_back({p + "6", 1});
    else if (mem_depth(t) == 32)
        for (auto &p : ps)
            out.push_back({p + "1", 0});
    // a RAM32M's / RAM64M's data input with no net: tied low, as ISE's map ties the D LUT's DI and DX (#2755)
    if (t == "RAM32M" || t == "RAM64M")
        for (auto &p : ps) {
            std::vector<std::string> pins = t == "RAM32M" ? std::vector<std::string>{"DI" + p + "<0>", "DI" + p + "<1>"}
                                                          : std::vector<std::string>{"DI" + p};
            for (auto &pin : pins)
                if (!c.has(pin))
                    for (auto &sp : mem_in_pins(c, ps, pin, -1))
                        out.push_back({sp, 0});
        }
    static const std::regex wewclk("WE|WCLK");
    for (auto &pn : c.pins) {
        std::string base = pn.first.substr(0, pn.first.find('<'));
        if (konst(pn.second) < 0 || c.is_output(base) || std::regex_match(pn.first, wewclk))
            continue;
        for (auto &sp : mem_in_pins(c, ps, pn.first, -1))
            out.push_back({sp, konst(pn.second)});
    }
    return out;
}

std::string mem_out_pin(const NCell &c, const std::vector<std::string> &ps, const std::string &pin)
{
    const std::string &t = c.type;
    auto [name, bit] = bit_of(pin);
    if (is_srl(t)) {
        if (pin == "Q")
            return ps.at(0);
        return ps.at(0) == "A" ? "DMUX" : "";   // Q15, Q31: the A LUT's cascade, by DMUX only
    }
    if (t == "RAM32M" || t == "RAM64M") {
        static const std::regex re("DO([A-D])");
        std::smatch m;
        if (!std::regex_match(name, m, re))
            return "";
        return t == "RAM32M" && bit == 0 ? m[1].str() + "MUX" : m[1].str();
    }
    auto parts = mem_layout(c, ps);
    char port = name == "DPO" ? 'r' : 'w';
    std::vector<Part> mine;
    for (auto &q : parts)
        if (q.port == port)
            mine.push_back(q);
    if (mine.size() == 1)
        return mine[0].p;
    if (mine.size() == 2) {
        bool ab = false;
        for (auto &q : mine)
            ab |= q.p == "A" || q.p == "B";
        return ab ? "AMUX" : "CMUX";
    }
    return "BMUX";
}

} // namespace s6

NEXTPNR_NAMESPACE_END
