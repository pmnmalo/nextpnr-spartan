# The 7-series' delays fitted to Vivado's (pmnmalo/Silinx#3516): Vivado 2025.2 placed and routed Silinx's netlists of
# its ten examples on the Basys 3 (xc7a35tcpg236-1, speed grade -1), then wrote each cell's and each routed connection's
# delay (write_sdf, the slow corner) and each route's nodes. Silinx's scripts/xc7-timing-fit.mjs fitted these numbers
# (Silinx's packages/silinx-fpga/research/xc7/timing-fit.json): the cells' arcs as Vivado's SDF gives them (min and max
# over the examples), and a routed connection's delay as the sum of its nodes' delays, one per wire class (a non-negative
# least-squares fit over 8343 connections, 95.8 ps rms).
#
# The chip database is built with these in place of Project X-Ray's RC model (each PIP's delay, input capacitance and
# output resistance, each node's capacitance and resistance) and of the slice's stub delays, where Vivado gave a value:
# nextpnr's Fmax on the examples was 1.13x to 1.96x Vivado's with them.

import re

# ---- the routing: a node's delay by its wire class (ps) ----

# Vivado's class of a node, by its wire's name (Silinx's scripts/xc7-timing-parse.mjs wireClass)
WIRE_CLASSES = [
    ("single", re.compile(r"^[NSEW][LR]1(BEG|END)")),
    ("double", re.compile(r"^(EE|WW|NN|SS|NE|NW|SE|SW)2(BEG|END|[A-Z])")),
    ("quad", re.compile(r"^(EE|WW|NN|SS|NE|NW|SE|SW)4(BEG|END|[A-Z])")),
    ("hex", re.compile(r"^(EE|WW|NN|SS|NE|NW|SE|SW)6(BEG|END|[A-Z])")),
    ("long", re.compile(r"^L[HV]B?(_L)?\d+")),
    ("output", re.compile(r"^LOGIC_OUTS")),
    ("imux", re.compile(r"^IMUX")),
    ("bypass", re.compile(r"^BYP")),
    ("fan", re.compile(r"^(G?FAN)")),
    ("ctrl", re.compile(r"^CTRL")),
    ("bounce", re.compile(r"BOUNCE")),
    ("clock", re.compile(r"^(G?CLK|HCLK|GCLK|CLK_|BUFG|CMT)")),
    ("const", re.compile(r"^(GND|VCC)_WIRE")),
]
SITE_TILE = re.compile(r"^(CLB|IOI|LIOI|RIOI|LIOB|RIOB|BRAM|DSP)")

# the fitted delay of each class (timing-fit.json's wires.classes, base 0); a class no route of the examples used
# (output, bounce, const): 0
WIRE_DELAY = {
    "single": 154.4, "double": 191.9, "quad": 326.7, "hex": 248.2, "long": 399.5, "output": 0, "imux": 272.4,
    "bypass": 74.8, "fan": 80.9, "ctrl": 90.8, "bounce": 0, "clock": 525.7, "const": 0, "site": 77.9, "other": 0,
}

# No PIP is given less than this (ps): a PIP of no delay (inside a site, a class the fit gave 0) leaves router 1 nothing
# to prefer a shorter route by, and on micro80 it ripped up the same arcs without end
MIN_PIP_DELAY = 10

def pip_delay(ps):
    return max(MIN_PIP_DELAY, round(ps))

def wire_class(tile_type, wire_name):
    for cls, pattern in WIRE_CLASSES:
        if pattern.search(wire_name):
            return cls
    return "site" if SITE_TILE.match(tile_type) else "other"

# A routed connection of the fabric passes, in Vivado's routes, three nodes of class "site" (the site pins' wires: 7304
# of the 8175 connections; the rest four to seven): two are given to the site's exit, one to the site's entrance. A
# dedicated cascade (the carry chain's COUT -> CIN, the block RAMs' and DSPs' cascades) has a delay of 0 in Vivado.
SITE_EXIT_DELAY = round(2 * WIRE_DELAY["site"])
SITE_ENTRANCE_DELAY = round(WIRE_DELAY["site"])
CASCADE_PIN = re.compile(r"^(COUT|CIN|CARRYCASC(IN|OUT)|MULTSIGN(IN|OUT)|[ABP]C(IN|OUT)\d*|CASCADE.*)$")

def is_cascade_pin(name):
    return CASCADE_PIN.match(name) is not None

# ---- the cells (ps: (min, max) over the examples, the slow corner) ----

# SLICE_LUTX: each input to O6 and O5
LUT = {
    "O6": {f"A{i}": (124, 124) for i in range(1, 7)},
    "O5": {"A1": (149, 156), "A2": (146, 157), "A3": (148, 153), "A4": (149, 153), "A5": (116, 120)},
}
# a LUT's route-through: the site's entrance, the LUT, the site's exit
LUT_ROUTETHRU_DELAY = SITE_ENTRANCE_DELAY + LUT["O6"]["A1"][1] + SITE_EXIT_DELAY

# SLICE_FFX: clock to Q, the setups (Vivado's SDF: D's is negative)
FF_CLK_Q = (303, 362)
FF_SETUP = {"D": (-60, -45), "CE": (109, 109), "SR": (428, 552)}

# CARRY4 (Vivado's CI is nextpnr's CIN)
CARRY4 = {
    "CIN": {"CO0": (254, 271), "CO1": (157, 157), "CO2": (228, 229), "CO3": (114, 117),
            "O0": (219, 235), "O1": (323, 348), "O2": (239, 256), "O3": (313, 331)},
    "CYINIT": {"CO0": (536, 561), "CO1": (494, 511), "CO2": (592, 608), "CO3": (580, 595),
               "O0": (482, 510), "O1": (598, 644), "O2": (584, 614), "O3": (642, 678)},
    "DI0": {"CO0": (327, 393), "CO1": (396, 488), "CO2": (474, 564), "CO3": (445, 550),
            "O1": (325, 436), "O2": (486, 596), "O3": (545, 659)},
    "DI1": {"CO1": (376, 456), "CO2": (459, 533), "CO3": (427, 520), "O2": (470, 566), "O3": (532, 629)},
    "DI2": {"CO2": (289, 366), "CO3": (299, 404), "O3": (367, 463)},
    "DI3": {"CO3": (305, 396)},
    "S0": {"CO0": (340, 340), "CO1": (433, 434), "CO2": (512, 514), "CO3": (489, 508),
           "O0": (223, 228), "O1": (400, 403), "O2": (520, 523), "O3": (582, 584)},
    "S1": {"CO1": (469, 472), "CO2": (548, 554), "CO3": (513, 528), "O1": (205, 210), "O2": (558, 558),
           "O3": (618, 623)},
    "S2": {"CO2": (292, 296), "CO3": (358, 376), "O2": (226, 228), "O3": (330, 330)},
    "S3": {"CO3": (354, 380), "O3": (227, 233)},
}

# SELMUX2_1, the F7 and F8 multiplexers' cell (MUXF7's I0, I1, S and MUXF8's: the smaller min, the larger max)
SELMUX2_1 = {"0": (98, 219), "1": (88, 225), "S0": (273, 314)}

# DSP48E1 with no register (AREG = BREG = … = PREG = 0, as Yosys maps a multiplier with no register around it): its
# inputs' arcs to its outputs, each bus's bits the same
DSP48E1_INPUTS = {"A": 30, "B": 18, "C": 48, "ALUMODE": 4, "CARRYIN": 0, "CARRYINSEL": 3, "INMODE": 5, "OPMODE": 7,
                  "PCIN": 48, "CARRYCASCIN": 0, "MULTSIGNIN": 0}
DSP48E1_OUTPUTS = {"P": 48, "PCOUT": 48, "CARRYOUT": 4, "CARRYCASCOUT": 0, "MULTSIGNOUT": 0, "PATTERNDETECT": 0,
                   "PATTERNBDETECT": 0}
DSP48E1_COMB = {
    "A": {"P": 3841, "PCOUT": 4036, "CARRYOUT": 3818, "CARRYCASCOUT": 4171, "MULTSIGNOUT": 3799, "PATTERNDETECT": 4406, "PATTERNBDETECT": 4406},
    "B": {"P": 3656, "PCOUT": 3851, "CARRYOUT": 3633, "CARRYCASCOUT": 3986, "MULTSIGNOUT": 3615, "PATTERNDETECT": 4221, "PATTERNBDETECT": 4221},
    "C": {"P": 1820, "PCOUT": 2016, "CARRYOUT": 1797, "CARRYCASCOUT": 2150, "MULTSIGNOUT": 1779, "PATTERNDETECT": 2386, "PATTERNBDETECT": 2386},
    "ALUMODE": {"P": 2098, "PCOUT": 2292, "CARRYOUT": 2074, "CARRYCASCOUT": 2427, "MULTSIGNOUT": 2055, "PATTERNDETECT": 2423, "PATTERNBDETECT": 2423},
    "CARRYIN": {"P": 1416, "PCOUT": 1613, "CARRYOUT": 1393, "CARRYCASCOUT": 1746, "MULTSIGNOUT": 1376, "PATTERNDETECT": 1982, "PATTERNBDETECT": 1982},
    "CARRYINSEL": {"P": 1910, "PCOUT": 2106, "CARRYOUT": 1888, "CARRYCASCOUT": 2241, "MULTSIGNOUT": 1870, "PATTERNDETECT": 2476, "PATTERNBDETECT": 2476},
    "INMODE": {"P": 3923, "PCOUT": 4119, "CARRYOUT": 3900, "CARRYCASCOUT": 4254, "MULTSIGNOUT": 3883, "PATTERNDETECT": 4489, "PATTERNBDETECT": 4489},
    "OPMODE": {"P": 2326, "PCOUT": 2522, "CARRYOUT": 2304, "CARRYCASCOUT": 2657, "MULTSIGNOUT": 2286, "PATTERNDETECT": 2892, "PATTERNBDETECT": 2892},
    "PCIN": {"P": 1518, "PCOUT": 1713, "CARRYOUT": 1494, "CARRYCASCOUT": 1848, "MULTSIGNOUT": 1478, "PATTERNDETECT": 2084, "PATTERNBDETECT": 2084},
    "CARRYCASCIN": {"P": 1147, "PCOUT": 1343, "CARRYOUT": 1124, "CARRYCASCOUT": 1478, "MULTSIGNOUT": 1106, "PATTERNDETECT": 1712, "PATTERNBDETECT": 1712},
    "MULTSIGNIN": {"P": 1518, "PCOUT": 1713, "CARRYOUT": 1494, "CARRYCASCOUT": 1848, "MULTSIGNOUT": 1478, "PATTERNDETECT": 2084, "PATTERNBDETECT": 2084},
}
# B -> BCOUT (the B input's cascade)
DSP48E1_B_BCOUT = 735

def bus_pins(name, width):
    return [name] if width == 0 else [f"{name}{i}" for i in range(width)]
