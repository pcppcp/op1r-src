#!/usr/bin/env python3
"""Compare isolated DSP cost with a prior source tree; NOT device CPU timing."""
from pathlib import Path
import argparse,hashlib,json,platform,statistics,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument("baseline",type=Path)
ap.add_argument("output",type=Path)
a=ap.parse_args()
result={}
with tempfile.TemporaryDirectory(prefix="vapora-benchmark-") as tmp:
    for label,source in [("baseline",a.baseline),("current",ROOT)]:
        binary=Path(tmp)/label
        subprocess.run(["clang","-std=c11","-O2","-I"+str(source),
                        str(ROOT/"tests/benchmark.c"),str(source/"vapora_dsp.c"),
                        "-o",str(binary)],check=True)
        runs=[list(map(float,subprocess.check_output([binary],text=True).split(",")))
              for _ in range(3)]
        result[label]={"median_seconds":[statistics.median(x) for x in zip(*runs)],
                       "runs_seconds":runs,
                       "dsp_sha256":hashlib.sha256((source/"vapora_dsp.c").read_bytes()).hexdigest()}
result["cases"]=["one held voice","three held voices","six held voices",
                 "six voices, Character changes","six voices, variable pitch"]
result["current_relative_to_baseline"]=[b/a for a,b in zip(result["baseline"]["median_seconds"],result["current"]["median_seconds"])]
result["host"]=platform.platform()
result["compiler"]=subprocess.check_output(["clang","--version"],text=True).splitlines()[0]
result["conditions"]="clang -O2; 30,000 blocks per case; median of three runs; deterministic Fifth Tide controls"
result["limitation"]="Isolated DSP on this host; excludes adapter, T2, FX, GUI and firmware. Does not predict Blackfin CPU percentages or deadline success."
a.output.write_text(json.dumps(result,indent=2)+"\n")
print(json.dumps(result,indent=2))
