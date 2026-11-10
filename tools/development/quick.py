import csv,collections,statistics as st,sys
def rows(path):
    with open(path) as f:
        r=csv.reader(f); hdr=next(r)
        for x in r:
            while len(x)>len(hdr): x=[x[0]+';'+x[1]]+x[2:]
            yield dict(zip(hdr,x))
if __name__=='__main__':
    d=collections.defaultdict(list)
    for r in rows(sys.argv[1]): d[r['engine']].append(r)
    print(f"{'engine':12s} {'mean':>7s} {'first':>7s} {'last':>7s} {'hit':>6s} {'grej':>7s} {'err':>4s}")
    for e,v in sorted(d.items(), key=lambda kv: st.median(float(x['mean_ns']) for x in kv[1])):
        f=lambda k: st.median(float(x[k]) for x in v)
        print(f"{e:12s} {f('mean_ns'):7.1f} {f('first_ns'):7.1f} {f('last_ns'):7.1f} {f('hit_ratio'):6.3f} {f('guard_rejects')+f('filter_pos'):7.0f} {max(int(x['errors']) for x in v):4d}")
