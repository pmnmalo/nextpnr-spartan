# The LUTs of a design placed by nextpnr-himbaechel-xilinx (its --write JSON), counted as Vivado's report_utilization
# counts them: a LUT site (a 6LUT and its 5LUT) holding logic counts once, a site holding only feed-throughs (route-thrus,
# which Vivado does not count) not at all.
#   python3 lut-count.py <placed.json>   ->   "<LUT cells> <feed-through cells> <LUT sites with logic> <LUT sites>"
import json, re, sys

d = json.load(open(sys.argv[1]))
mod = max(d['modules'].values(), key=lambda m: len(m['cells']))
sites, cells, feeds = {}, 0, 0
for name, c in mod['cells'].items():
    bel = c.get('attributes', {}).get('NEXTPNR_BEL', '')
    if c['type'] != 'SLICE_LUTX' or not bel:
        continue
    # A feed-through from the packer: XilinxPacker::feed_through_lut's "<net>$LUT<n>", or a MUXCY entry's "$feed$zero"
    feed = re.search(r'\$LUT\d+$', name) is not None or name.endswith('$feed$zero')
    site = bel.split('.')[0] + '.' + bel.split('.')[1][0]      # X10Y138/SLICE_X0Y0.C6LUT -> X10Y138/SLICE_X0Y0.C
    sites.setdefault(site, False)
    sites[site] |= not feed
    cells += 1
    feeds += feed
print(cells, feeds, sum(sites.values()), len(sites))
