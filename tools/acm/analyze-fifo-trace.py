"""Compare completed FIFO transactions without conflating frame boundaries and values."""
import argparse,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('reference');p.add_argument('candidate');a=p.parse_args()
def load(path):return [line.split() for line in Path(path).read_text().splitlines() if line.split()[1] in ('w','r')]
r,c=load(a.reference),load(a.candidate);n=min(len(r),len(c))
def first(start):return next((i for i in range(n) if r[i][start:5]!=c[i][start:5]),None)
def timestamp(row):return int(row[5].replace(':',''),16)
print(json.dumps({'reference_transactions':len(r),'candidate_transactions':len(c),'first_frame_or_value_difference':first(0),'first_pc_or_value_difference':first(1),'last_common_event_delta_ns':(timestamp(c[n-1])-timestamp(r[n-1]))/1e9 if n else None},indent=2))
