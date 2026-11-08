# Spike (Silinx #2305): a FASM file's canonical features (one bit a line, sorted), for the round trip nextpnr -> .bit -> bit2fasm
import sys, fasm
out = set()
for line in fasm.parse_fasm_filename(sys.argv[1]):
    if line.set_feature:
        for f in fasm.canonical_features(line.set_feature):
            out.add(fasm.set_feature_to_str(f))
print('\n'.join(sorted(out)))
