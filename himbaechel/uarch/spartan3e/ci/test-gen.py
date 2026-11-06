# The chip database generator on a description that lacks what Silinx's measured descriptions lack
# (pmnmalo/Silinx-ISE#2502): the report's EMPTY_* tiles and the pins ISE was never seen to use (here the SLICEMs'
# SHIFTIN and SHIFTOUT). It must still write the database, with the same sites, an empty tile type in the empty
# places, and no BEL pin on the missing pins; the description as it is gives no empty tile type.
#   python3 test-gen.py <device graph .json.gz> <work dir>
import gzip, json, os, subprocess, sys
from os import path

src, work = sys.argv[1], sys.argv[2]
gen = path.join(path.dirname(path.abspath(__file__)), "..", "gen", "s3e_gen.py")
os.makedirs(work, exist_ok=True)
with gzip.open(src, "rt") as f:
    p = json.load(f)
empty = [t for t in p["tiles"] if p["templates"][t[3]]["type"].startswith("EMPTY")]
assert empty and all(not p["templates"][t[3]]["wires"] for t in empty), "the EMPTY_* tiles: some, with no wires"
p["tiles"] = [t for t in p["tiles"] if t not in empty]
gone = 0
for tp in p["templates"]:
    for s in tp["sites"]:
        if s["type"] == "SLICEM":
            n = len(s["pins"])
            s["pins"] = [q for q in s["pins"] if q[0] not in ("SHIFTIN", "SHIFTOUT")]
            gone += n - len(s["pins"])
assert gone, "SLICEM pins left out"
measured = path.join(work, "measured.json.gz")
with gzip.open(measured, "wt") as f:
    json.dump(p, f)

def run(dev, name):
    bba, sites = path.join(work, f"{name}.bba"), path.join(work, f"{name}.json")
    r = subprocess.run([sys.executable, gen, dev, bba, "--sites", sites], capture_output=True, text=True)
    assert r.returncode == 0, f"{name}: {r.stderr[-2000:]}"
    with open(bba) as f:
        text = f.read()
    with open(sites) as f:
        return text, json.load(f)

a, sites_a = run(src, "report")
b, sites_b = run(measured, "measured")
assert sites_a == sites_b, "the same sites"
assert "EMPTY_MEASURED" not in a and "EMPTY_MEASURED" in b, "an empty tile type only where tiles are missing"
assert "SHIFTIN" in a, "the SLICEMs' SHIFTIN where the description has it"
print(f"ok   generator: {len(empty)} empty places, {gone} SLICEM pins left out, the same {len(sites_a)} sites")
