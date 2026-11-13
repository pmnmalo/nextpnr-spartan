from os import path
import sys
import argparse
import xilinx_device
import filters
import struct
import parse_sdf
import timing_fit
sys.path.append(path.join(path.dirname(__file__), "../../.."))
from himbaechel_dbgen.chip import *

def lookup_port_type(t):
    if t == "INPUT": return PinType.INPUT
    elif t == "OUTPUT": return PinType.OUTPUT
    elif t == "BIDIR": return PinType.INOUT
    else: assert False

def gen_bel_name(site, bel_name):
    prim_st = site.primary.site_type()
    if prim_st in ("IOB33M", "IOB33S", "IOB33", "IOB18M", "IOB18S", "IOB18"):
        return f"{site.site_type()}.{bel_name}"
    else:
        return f"{bel_name}"

@dataclass
class PipClass(Enum):
    TILE_ROUTING = 0
    SITE_ENTRANCE = 1
    SITE_EXIT = 2
    SITE_INTERNAL = 3
    LUT_PERMUTATION = 4
    LUT_ROUTETHRU = 5
    CONST_DRIVER = 6

@dataclass
class PipExtraData(BBAStruct):
    site_key: int = -1
    bel_name: IdString = field(default_factory=IdString)
    pip_config: int = 0

    def serialise_lists(self, context: str, bba: BBAWriter):
        pass
    def serialise(self, context: str, bba: BBAWriter):
        bba.u32(self.site_key)
        bba.u32(self.bel_name.index)
        bba.u32(self.pip_config)

@dataclass
class BelExtraData(BBAStruct):
    name_in_site: IdString

    def serialise_lists(self, context: str, bba: BBAWriter):
        pass
    def serialise(self, context: str, bba: BBAWriter):
        bba.u32(self.name_in_site.index)

@dataclass
class SiteInst(BBAStruct):
    name_prefix: IdString
    site_x: int
    site_y: int
    rel_x: int
    rel_y: int
    int_x: int
    int_y: int
    variants: list[IdString] = field(default_factory=list)

    def serialise_lists(self, context: str, bba: BBAWriter):
        bba.label(f"{context}_variants")
        for i, variant in enumerate(self.variants):
            bba.u32(variant.index)
    def serialise(self, context: str, bba: BBAWriter):
        bba.u32(self.name_prefix.index)
        bba.u16(self.site_x)
        bba.u16(self.site_y)
        bba.u16(self.rel_x)
        bba.u16(self.rel_y)
        bba.u16(self.int_x)
        bba.u16(self.int_y)
        bba.slice(f"{context}_variants", len(self.variants))

@dataclass
class TileExtraData(BBAStruct):
    name_prefix: IdString
    tile_x: int
    tile_y: int
    sites: list[SiteInst] = field(default_factory=list)

    def serialise_lists(self, context: str, bba: BBAWriter):
        for i, site in enumerate(self.sites):
            site.serialise_lists(f"{context}_si{i}", bba)
        bba.label(f"{context}_sites")
        for i, site in enumerate(self.sites):
            site.serialise(f"{context}_si{i}", bba)
    def serialise(self, context: str, bba: BBAWriter):
        bba.u32(self.name_prefix.index)
        bba.u16(self.tile_x)
        bba.u16(self.tile_y)
        bba.slice(f"{context}_sites", len(self.sites))

# A PIP's timing class: the class of the wire it drives (its node's), whose delay Vivado's routes gave (timing_fit.py)
def timing_pip_class(ch: Chip, pip: xilinx_device.PIP):
    cls = timing_fit.wire_class(pip.tile.tile_type(), pip.dst_wire().name())
    tcls = f"WIRE_{cls.upper()}"
    if tcls not in seen_pip_timings:
        ch.timing.set_pip_class("DEFAULT", tcls, delay=TimingValue(timing_fit.pip_delay(timing_fit.WIRE_DELAY[cls])), is_buffered=True)
        seen_pip_timings.add(tcls)
    return tcls

seen_pip_timings = set()

def import_tiletype(ch: Chip, tile: xilinx_device.Tile):
    tile_type = tile.tile_type()
    if tile.x == 0 and tile.y == 0:
        assert tile_type == "NULL"
        tile_type = "NULL_CORNER"
    tt = ch.create_tile_type(tile_type)
    # import tile wires
    for wire in tile.wires():
        nw = tt.create_wire(wire.name(), wire.intent())
        nw.flags = -1 # not a site wire
        if wire.is_gnd():
            nw.const_value = ch.strs.id("GND")
        if wire.is_vcc():
            nw.const_value = ch.strs.id("VCC")
    if tile_type == "NULL_CORNER":
        # pseudo ground bels
        tt.create_wire(f"GND", "GND", const_value="GND")
        tt.create_wire(f"VCC", "VCC", const_value="VCC")

        gnd = tt.create_bel(f"PSEUDO_GND", f"PSEUDO_GND", z=0)
        gnd.site = -1
        gnd.extra_data = BelExtraData(name_in_site=ch.strs.id("PSEUDO_GND"))
        gnd.flags |= BEL_FLAG_GLOBAL
        tt.add_bel_pin(gnd, "Y", "GND", PinType.OUTPUT)

        vcc = tt.create_bel(f"PSEUDO_VCC", f"PSEUDO_VCC", z=1)
        vcc.site = -1
        vcc.extra_data = BelExtraData(name_in_site=ch.strs.id("PSEUDO_VCC"))
        vcc.flags |= BEL_FLAG_GLOBAL
        tt.add_bel_pin(vcc, "Y", "VCC", PinType.OUTPUT)
    def add_pip(src_wire, dst_wire, pip_class=PipClass.TILE_ROUTING, timing="",
        site_key=-1, bel_name="", pip_config=0):
        np = tt.create_pip(src_wire, dst_wire, timing)
        np.flags = pip_class.value
        np.extra_data = PipExtraData(site_key=site_key, bel_name=ch.strs.id(bel_name),
            pip_config=pip_config)

    def lookup_site_wire(sw):
        canon_name = f"{sw.site.rel_name()}.{sw.name()}"
        if not tt.has_wire(canon_name):
            nw = tt.create_wire(name=canon_name,
                type="INTENT_SITE_GND" if sw.name() == "GND_WIRE" else "INTENT_SITE_WIRE")
            nw.flags = sw.site.primary.index
        return canon_name

    def add_site_io_pip(pin):
        pn = pin.name()
        s = pin.site
        site_key = (s.index << 8)
        if pin.tile_wire() is None:
            return None
        # TODO: timing class
        if pin.dir() in ("OUTPUT", "BIDIR"):
            if s.primary.site_type() == "IPAD" and pn == "O":
                return None
            # if s.rel_xy()[0] == 0 and s.primary.site_type() == "SLICEL" and pin.site_wire().name() == "A":
            #     # Add ground pip
            #     # self.add_pseudo_pip(self.row_gnd_wire_index, self.sitewire_to_tilewire(pin.site_wire()),
            #     #    pip_type=NextpnrPipType.CONST_DRIVER)
            #     pass
            add_pip(lookup_site_wire(pin.site_wire()), pin.tile_wire().name(),
                pip_class=PipClass.SITE_EXIT, timing="SITE_CASCADE" if timing_fit.is_cascade_pin(pn) else "SITE_EXIT")
        else:
            if s.site_type() in ("SLICEL", "SLICEM"):
                # Add permuation pseudo-pips for LUT inputs
                swn = pin.site_wire().name()
                if len(swn) == 2 and swn[0] in "ABCDEFGH" and swn[1] in "123456":
                    i = int(swn[1])
                    for j in range(1, 7):
                        if (i == 6) != (j == 6):
                            continue # don't allow permutation of input 6
                        pip_config = ("ABCDEFGH".index(swn[0]) << 8) | ((j - 1) << 4) | (i - 1)
                        if s.rel_xy()[0] == 1:
                            pip_config |= (4 << 8)
                        add_pip(s.pin(f"{swn[0]}{j}").tile_wire().name(), lookup_site_wire(pin.site_wire()),
                            pip_class=PipClass.LUT_PERMUTATION,
                            pip_config=pip_config,
                            site_key=(s.index << 8),
                            timing="SITE_ENTRANCE"
                        )
                    # Add LUT route-through pseudo-pip
                    rt_config = ("ABCDEFGH".index(swn[0]) << 8)
                    if s.rel_xy()[0] == 1:
                        rt_config |= (4 << 8)
                    rt_config |= ((i - 1) << 1)
                    add_pip(pin.tile_wire().name(), s.pin(f"{swn[0]}").tile_wire().name(),
                            pip_class=PipClass.LUT_ROUTETHRU,
                            pip_config=rt_config,
                            site_key=(s.index << 8),
                            timing="LUT_ROUTETHRU"
                        )

                    return

            add_pip(pin.tile_wire().name(), lookup_site_wire(pin.site_wire()),
                pip_class=PipClass.SITE_ENTRANCE, timing="SITE_CASCADE" if timing_fit.is_cascade_pin(pn) else "SITE_ENTRANCE")

    # TODO: ground/vcc
    tile_wire_count = len(tt.wires)
    for site in tile.sites():
        seen_pins = set()
        for variant_idx, variant in enumerate(site.available_variants()):
            if variant in ("FIFO36E1", ): #unsupported atm
                continue
            sv = site.variant(variant)
            variant_key = (site.index << 8) | (variant_idx & 0xFF)
            # Import site bels
            for bel in sv.bels():
                z = filters.get_bel_z_override(bel, len(tt.bels))
                # Overriden z of -1 means we skip this bel
                if z == -1:
                    continue
                bel_name = gen_bel_name(sv, bel.name())
                nb = tt.create_bel(name=f"{site.rel_name()}.{bel_name}", type=filters.get_bel_type_override(bel.bel_type()), z=z)
                nb.site = variant_key
                nb.extra_data = BelExtraData(name_in_site=ch.strs.id(bel_name))
                if bel.bel_class() == "RBEL": nb.flags |= 2
                if filters.is_global_bel(bel):
                    nb.flags |= BEL_FLAG_GLOBAL
                # TODO: extra data (site etc)
                for pin in bel.pins():
                    tt.add_bel_pin(nb, pin.name, lookup_site_wire(pin.site_wire()), lookup_port_type(pin.dir()))
            # Import site pins
            for pin in sv.pins():
                pin_key = (pin.name(), pin.site_wire().name())
                if pin_key not in seen_pins:
                    add_site_io_pip(pin)
                    seen_pins.add(pin_key)
            # Import site pips
            for site_pip in sv.pips():
                if "LUT" in site_pip.bel().bel_type():
                    continue # ignore site LUT route-throughs
                bel_name = site_pip.bel().name()
                bel_pin = site_pip.bel_input()
                if (bel_name == "ADI1MUX" and bel_pin == "BDI1") or \
                    (bel_name == "BDI1MUX" and bel_pin == "DI") or \
                    (bel_name == "CDI1MUX" and bel_pin == "DI") or \
                    (bel_name.startswith("TFBUSED")) or \
                    (bel_name == "OMUX") or \
                    (bel_name == "IDELMUXE3"):

                    continue
                add_pip(lookup_site_wire(site_pip.src_wire()),
                    lookup_site_wire(site_pip.dst_wire()),
                    pip_class=PipClass.SITE_INTERNAL,
                    site_key=variant_key,
                    bel_name=bel_name,
                    pip_config=ch.strs.id(bel_pin).index,
                    timing="SITE_NULL"
                )
    # Import tile pips
    for pip in tile.pips():
        if not filters.include_pip(tile.tile_type(), pip):
            continue
        tcls = timing_pip_class(ch, pip)
        add_pip(pip.src_wire().name(), pip.dst_wire().name(), pip_class=PipClass.TILE_ROUTING, timing=tcls,
            pip_config=1 if pip.is_route_thru() else 0)
        # TODO: extra data, route-through flag
        if pip.is_bidi():
            add_pip(pip.dst_wire().name(), pip.src_wire().name(), pip_class=PipClass.TILE_ROUTING, timing=tcls,
                pip_config=1 if pip.is_route_thru() else 0)

def import_sdf_timings(variant, sdfcell):
    # TODO: anything other than comb
    for entry in sdfcell.entries:
        if isinstance(entry, parse_sdf.IOPath):
            variant.add_comb_arc(entry.from_pin, entry.to_pin,
                TimingValue(int(min(entry.rising.minv, entry.falling.minv)*1000),
                    int(max(entry.rising.maxv, entry.falling.maxv)*1000)))

def import_bram_timings(timing, sdf):
    def import_bus_sethold(cell, port, width, clock, entry):
        for i in range(width):
            cell.add_setup_hold(clock, f"{port}{i}", ClockEdge.RISING, TimingValue(int(entry.setup.minv*1000), int(entry.setup.maxv*1000)),
                TimingValue(int(entry.hold.minv*1000), int(entry.hold.maxv*1000)))

    def import_bus_clkq(cell, port, width, clock, entry):
        for i in range(width):
            cell.add_clock_out(clock, f"{port}{i}", ClockEdge.RISING, TimingValue(int(min(entry.rising.minv, entry.falling.minv)*1000),
                int(max(entry.rising.maxv, entry.falling.maxv)*1000)))


    def import_pin_sethold(cell, port, clock, entry):
        cell.add_setup_hold(clock, f"{port}", ClockEdge.RISING, TimingValue(int(entry.setup.minv*1000), int(entry.setup.maxv*1000)),
                TimingValue(int(entry.hold.minv*1000), int(entry.hold.maxv*1000)))

    for wsdp, rsdp in ((False, False), (False, True), (True, False), (True, True)):
        bram18 = timing.add_cell_variant("DEFAULT", f"RAMB18E1_RAMB18E1_{'WSDP' if wsdp else 'WTDP'}_{'RSDP' if rsdp else 'RTDP'}")

        for entry in sdf.cells[("RAMBFIFO36E1", "RAMBFIFO36E1")].entries:
            if isinstance(entry, parse_sdf.SetupHoldCheck):
                if entry.pin == "ADDRAU": import_bus_sethold(bram18, "ADDRARDADDR", 14, "CLKARDCLK", entry)
                if entry.pin == "ADDRBU": import_bus_sethold(bram18, "ADDRBWRADDR", 14, "CLKBWRCLK", entry)
                if entry.pin == "WEAU": import_bus_sethold(bram18, "WEA", 4, "CLKARDCLK", entry)
                if entry.pin == "WEBU": import_bus_sethold(bram18, "WEBWE", 8, "CLKBWRCLK", entry)
        for entry in sdf.cells[("RAMBFIFO36E1_ISFIFO_FALSE", "RAMBFIFO36E1")].entries:
            if isinstance(entry, parse_sdf.SetupHoldCheck):
                if entry.pin == "ENARDENU": import_pin_sethold(bram18, "ENARDEN", "CLKARDCLK", entry)
                if entry.pin == "ENBWRENU": import_pin_sethold(bram18, "ENBWREN", "CLKBWRCLK", entry)
                if entry.pin == "RSTRAMAU": import_pin_sethold(bram18, "RSTRAMARSTRAM", "CLKARDCLK", entry)
                if entry.pin == "RSTRAMBU": import_pin_sethold(bram18, "RSTRAMB", "CLKBWRCLK", entry)
        for entry in sdf.cells[("RAMBFIFO36E1RAM_MODE_RAMB18TDP_U_WRITE_MODE_U_NC_EN_ECC_READ_FALSE_EN_ECC_WRITE_FALSE", "RAMBFIFO36E1")].entries:
            if isinstance(entry, parse_sdf.SetupHoldCheck):
                if entry.pin == "DIADIU": import_bus_sethold(bram18, "DIADI", 16, "CLKBWRCLK" if wsdp else "CLKARDCLK", entry)
                if entry.pin == "DIBDIU": import_bus_sethold(bram18, "DIBDI", 16, "CLKBWRCLK", entry)
                if entry.pin == "DIPADIPU": import_bus_sethold(bram18, "DIPADIP", 2, "CLKBWRCLK" if wsdp else "CLKARDCLK", entry)
                if entry.pin == "DIPBDIPU": import_bus_sethold(bram18, "DIPBDIP", 2, "CLKBWRCLK", entry)
        for entry in sdf.cells[("RAMBFIFO36E1RAM_MODE_U_RAMB18SDP_U_DOA_REG_U_0_EN_ECC_READ_FALSE", "RAMBFIFO36E1")].entries:
            if isinstance(entry, parse_sdf.IOPath):
                if entry.to_pin == "DOADOU": import_bus_clkq(bram18, "DOADO", 16, "CLKARDCLK", entry)
                if entry.to_pin == "DOPADOPU": import_bus_clkq(bram18, "DOPADOP", 2, "CLKARDCLK", entry)
        for entry in sdf.cells[("RAMBFIFO36E1RAM_MODE_U_RAMB18TDP_U_DOB_REG_U_0_EN_ECC_READ_FALSE", "RAMBFIFO36E1")].entries:
            if isinstance(entry, parse_sdf.IOPath):
                if entry.to_pin == "DOBDOU": import_bus_clkq(bram18, "DOBDO", 16, "CLKARDCLK" if rsdp else "CLKBWRCLK", entry)
                if entry.to_pin == "DOPBDOPU": import_bus_clkq(bram18, "DOPBDOP", 2, "CLKARDCLK" if rsdp else "CLKBWRCLK", entry)

        bram36 = timing.add_cell_variant("DEFAULT", f"RAMB36E1_RAMB36E1_{'WSDP' if wsdp else 'WTDP'}_{'RSDP' if rsdp else 'RTDP'}")

        for u in ('L', 'U'):
            for entry in sdf.cells[("RAMBFIFO36E1", "RAMBFIFO36E1")].entries:
                if isinstance(entry, parse_sdf.SetupHoldCheck):
                    if entry.pin == "ADDRAU": import_bus_sethold(bram36, f"ADDRARDADDR{u}", 15 if u == 'L' else 14, "CLKARDCLK", entry)
                    if entry.pin == "ADDRBU": import_bus_sethold(bram36, f"ADDRBWRADDR{u}", 15 if u == 'L' else 14, "CLKBWRCLK", entry)
                    if entry.pin == "WEAU": import_bus_sethold(bram36, f"WEA{u}", 4, f"CLKARDCLK{u}", entry)
                    if entry.pin == "WEBU": import_bus_sethold(bram36, f"WEBWE{u}", 8, f"CLKBWRCLK{u}", entry)
            for entry in sdf.cells[("RAMBFIFO36E1_ISFIFO_FALSE", "RAMBFIFO36E1")].entries:
                if isinstance(entry, parse_sdf.SetupHoldCheck):
                    if entry.pin == "ENARDENU": import_pin_sethold(bram36, f"ENARDEN{u}", f"CLKARDCLK{u}", entry)
                    if entry.pin == "ENBWRENU": import_pin_sethold(bram36, f"ENBWREN{u}", f"CLKBWRCLK{u}", entry)
                    if entry.pin == "RSTRAMAU": import_pin_sethold(bram36, f"RSTRAMARSTRAM{u}", f"CLKARDCLK{u}", entry)
                    if entry.pin == "RSTRAMBU": import_pin_sethold(bram36, f"RSTRAMB{u}", f"CLKBWRCLK{u}", entry)
        for entry in sdf.cells[("RAMBFIFO36E1RAM_MODE_RAMB18TDP_U_WRITE_MODE_U_NC_EN_ECC_READ_FALSE_EN_ECC_WRITE_FALSE", "RAMBFIFO36E1")].entries:
            if isinstance(entry, parse_sdf.SetupHoldCheck):
                if entry.pin == "DIADIU": import_bus_sethold(bram36, "DIADI", 32, "CLKBWRCLK" if wsdp else "CLKARDCLK", entry)
                if entry.pin == "DIBDIU": import_bus_sethold(bram36, "DIBDI", 32, "CLKBWRCLK", entry)
                if entry.pin == "DIPADIPU": import_bus_sethold(bram36, "DIPADIP", 4, "CLKBWRCLK" if wsdp else "CLKARDCLK", entry)
                if entry.pin == "DIPBDIPU": import_bus_sethold(bram36, "DIPBDIP", 4, "CLKBWRCLK", entry)
        for entry in sdf.cells[("RAMBFIFO36E1RAM_MODE_U_RAMB18SDP_U_DOA_REG_U_0_EN_ECC_READ_FALSE", "RAMBFIFO36E1")].entries:
            if isinstance(entry, parse_sdf.IOPath):
                if entry.to_pin == "DOADOU": import_bus_clkq(bram36, "DOADO", 32, "CLKARDCLK", entry)
                if entry.to_pin == "DOPADOPU": import_bus_clkq(bram36, "DOPADOP", 4, "CLKARDCLK", entry)
        for entry in sdf.cells[("RAMBFIFO36E1RAM_MODE_U_RAMB18TDP_U_DOB_REG_U_0_EN_ECC_READ_FALSE", "RAMBFIFO36E1")].entries:
            if isinstance(entry, parse_sdf.IOPath):
                if entry.to_pin == "DOBDOU": import_bus_clkq(bram36, "DOBDO", 32, "CLKARDCLK" if rsdp else "CLKBWRCLK", entry)
                if entry.to_pin == "DOPBDOPU": import_bus_clkq(bram36, "DOPBDOP", 4, "CLKARDCLK" if rsdp else "CLKBWRCLK", entry)

def main():
    xlbase = path.join(path.dirname(path.realpath(__file__)), "..")

    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", help="Project X-Ray device database path for current family (e.g. ../prjxray-db/artix7)", type=str, required=True)
    parser.add_argument("--metadata", help="nextpnr-xilinx site metadata root", type=str, default=path.join(xlbase, "meta", "artix7"))
    parser.add_argument("--device", help="name of device to export", type=str, required=True)
    parser.add_argument("--constids", help="name of nextpnr constids file to read", type=str, default=path.join(xlbase, "constids.inc"))
    parser.add_argument("--bba", help="bba file to write", type=str, required=True)
    args = parser.parse_args()

    # Init database paths
    metadata_root = args.metadata
    xraydb_root = args.xray
    if "xc7z" in args.device:
        metadata_root = metadata_root.replace("artix7", "zynq7")
        xraydb_root = xraydb_root.replace("artix7", "zynq7")
    if "xc7k" in args.device:
        metadata_root = metadata_root.replace("artix7", "kintex7")
        xraydb_root = xraydb_root.replace("artix7", "kintex7")
    if "xc7s" in args.device:
        metadata_root = metadata_root.replace("artix7", "spartan7")
        xraydb_root = xraydb_root.replace("artix7", "spartan7")
    # Load prjxray device data
    d = xilinx_device.import_device(args.device, xraydb_root, metadata_root)
    # Init constant ids
    ch = Chip("xilinx", args.device, d.width, d.height)
    ch.strs.read_constids(path.join(path.dirname(__file__), args.constids))
    ch.set_speed_grades(["DEFAULT", ]) # TODO: figure out how speed grades are supposed to work in prjxray
    # Import tile types
    for tile in d.tiles:
        tile_type = tile.tile_type()
        if tile.x == 0 and tile.y == 0:
            assert tile_type == "NULL"
            # location for pseudo gnd/vcc bels
            tile_type = "NULL_CORNER"
        if tile_type not in ch.tile_type_idx:
            import_tiletype(ch, tile)
        ti = ch.set_tile_type(tile.x, tile.y, tile_type)
        prefix, tx, ty = tile.split_name()
        ti.extra_data = TileExtraData(ch.strs.id(prefix), tx, ty)
        for site in tile.sites():
            ti.extra_data.sites.append(SiteInst(
                name_prefix=ch.strs.id(site.prefix),
                site_x=site.grid_xy[0], site_y=site.grid_xy[1],
                rel_x=site.rel_xy()[0], rel_y=site.rel_xy()[1],
                int_x=tile.interconn_xy[0], int_y=tile.interconn_xy[1],
                variants=[ch.strs.id(v) for v in site.available_variants()]
            ))
    # Import nodes
    seen_nodes = set()
    tt_used_wires = {}
    print("Processing nodes...")
    for row in range(d.height):
        # TODO: GND and VCC row connectivity...
        for col in range(d.width):
            t = d.tiles_by_xy[col, row]
            for w in t.wires():
                n = w.node()
                uid = n.unique_index()
                if uid in seen_nodes:
                    continue
                if len(n.wires) > 1:
                    node_wires = []
                    # Add interconnect tiles first for better delay estimates in nextpnr
                    for j in range(2):
                        for w in n.wires:
                            if (w.tile.tile_type() in ("INT", "INT_L", "INT_R")) != (j == 0):
                                continue
                            if w.tile.tile_type() not in tt_used_wires:
                                tt_used_wires[w.tile.tile_type()] = w.tile.used_wire_indices()
                            # prune wires from nodes that just pass through a tile without connecting to
                            # bels/pips, because nextpnr doesn't care about these
                            if w.index not in tt_used_wires[w.tile.tile_type()]:
                                continue
                            node_wires.append(NodeWire(w.tile.x, w.tile.y, w.index))
                    if len(node_wires) > 1:
                        # (a node's delay is its driving PIP's: timing_pip_class)
                        ch.add_node(node_wires)
                    del node_wires
                seen_nodes.add(uid)
    # The site's PIPs (timing_fit.py): its exit and entrance, a LUT's route-through; inside the site and a dedicated
    # cascade's, the least (Vivado's SDF gives them nothing: the cells' arcs hold it)
    ch.timing.set_pip_class("DEFAULT", "SITE_NULL", delay=TimingValue(timing_fit.MIN_PIP_DELAY))
    ch.timing.set_pip_class("DEFAULT", "SITE_EXIT", delay=TimingValue(timing_fit.SITE_EXIT_DELAY))
    ch.timing.set_pip_class("DEFAULT", "SITE_ENTRANCE", delay=TimingValue(timing_fit.SITE_ENTRANCE_DELAY))
    ch.timing.set_pip_class("DEFAULT", "SITE_CASCADE", delay=TimingValue(timing_fit.MIN_PIP_DELAY))
    ch.timing.set_pip_class("DEFAULT", "LUT_ROUTETHRU", delay=TimingValue(timing_fit.LUT_ROUTETHRU_DELAY))
    # The slice's cells: Vivado's delays (timing_fit.py); the LUT RAM's write setups, stubs
    lut = ch.timing.add_cell_variant("DEFAULT", "SLICE_LUTX")
    for out, arcs in timing_fit.LUT.items():
        for pin, (lo, hi) in arcs.items():
            lut.add_comb_arc(pin, out, TimingValue(lo, hi))
    for i in range(1, 8+1):
        lut.add_setup_hold("CLK", f"WA{i}", ClockEdge.RISING, TimingValue(100, 500), TimingValue(100, 200))
    lut.add_setup_hold("CLK", "WE", ClockEdge.RISING, TimingValue(100, 600), TimingValue(100, 100))
    lut.add_setup_hold("CLK", "DI1", ClockEdge.RISING, TimingValue(100, 100), TimingValue(100, 100))
    ff = ch.timing.add_cell_variant("DEFAULT", "SLICE_FFX")
    ff.add_setup_hold("CK", "CE", ClockEdge.RISING, TimingValue(*timing_fit.FF_SETUP["CE"]), TimingValue(0, 0))
    ff.add_setup_hold("CK", "SR", ClockEdge.RISING, TimingValue(*timing_fit.FF_SETUP["SR"]), TimingValue(0, 0))
    ff.add_setup_hold("CK", "D", ClockEdge.RISING, TimingValue(*timing_fit.FF_SETUP["D"]), TimingValue(200, 200))
    ff.add_clock_out("CK", "Q", ClockEdge.RISING, TimingValue(*timing_fit.FF_CLK_Q))

    # The F7 / F8 multiplexers and the carry chain: Vivado's delays (timing_fit.py); a CARRY4 arc Vivado did not give,
    # Project X-Ray's SDF
    mux = ch.timing.add_cell_variant("DEFAULT", "SELMUX2_1")
    for pin, (lo, hi) in timing_fit.SELMUX2_1.items():
        mux.add_comb_arc(pin, "OUT", TimingValue(lo, hi))
    timings_root = xraydb_root
    if "kintex7" in xraydb_root: # TODO: missing
        timings_root = xraydb_root.replace("kintex7", "artix7")
    slicem_sdf = parse_sdf.parse_sdf_file(path.join(timings_root, "timings", "slicem.sdf"))
    carry_arcs = {}
    for entry in slicem_sdf.cells[("CARRY4", "SLICEM")].entries:
        if isinstance(entry, parse_sdf.IOPath):
            carry_arcs[(entry.from_pin, entry.to_pin)] = (int(min(entry.rising.minv, entry.falling.minv)*1000),
                int(max(entry.rising.maxv, entry.falling.maxv)*1000))
    for src, arcs in timing_fit.CARRY4.items():
        for dst, value in arcs.items():
            carry_arcs[(src, dst)] = value
    carry = ch.timing.add_cell_variant("DEFAULT", "CARRY4")
    for (src, dst), (lo, hi) in sorted(carry_arcs.items()):
        carry.add_comb_arc(src, dst, TimingValue(lo, hi))

    # A DSP48E1 with no register (XilinxImpl::assign_cell_info chooses it): Vivado's delays (timing_fit.py)
    dsp = ch.timing.add_cell_variant("DEFAULT", "DSP48E1_DSP48E1_COMB")
    for src, width in timing_fit.DSP48E1_INPUTS.items():
        for dst, delay in timing_fit.DSP48E1_COMB[src].items():
            for i in timing_fit.bus_pins(src, width):
                for o in timing_fit.bus_pins(dst, timing_fit.DSP48E1_OUTPUTS[dst]):
                    dsp.add_comb_arc(i, o, TimingValue(delay))
    for i in range(18):
        dsp.add_comb_arc(f"B{i}", f"BCOUT{i}", TimingValue(timing_fit.DSP48E1_B_BCOUT))

    import_bram_timings(ch.timing, parse_sdf.parse_sdf_file(path.join(timings_root, "timings", "BRAM_L.sdf")))

    # Import package pins
    for package_name, package in sorted(d.packages.items(), key=lambda x:x[0]):
        pkg = ch.create_package(package_name)
        for pin, site in sorted(package.pin_map.items(), key=lambda x:x[0]):
            site_data = d.sites_by_name[site]
            bel_name = gen_bel_name(site_data, "PAD")
            tile_type = ch.tile_type_at(site_data.tile.x, site_data.tile.y)
            pkg.create_pad(pin, f"X{site_data.tile.x}Y{site_data.tile.y}",
            f"{site_data.rel_name()}.{bel_name}",
            "", 0) # TODO: bank
    ch.write_bba(args.bba)

if __name__ == '__main__':
    main()
