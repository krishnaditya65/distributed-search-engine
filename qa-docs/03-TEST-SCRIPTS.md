# distributed-search-engine — Test Scripts (reference, NOT executed)

Request/response shapes are now from source (`coordinator/internal/api`): create-index `{shards, replicas, mappings:[{field,type,vector_dim}]}`; bulk `{ops:[{type, doc_id, body}]}` → `{results:[{doc_id,success,error}]}`; search `{query:<string>, fields:[...], filter:{term:[{field,value}],range:[{field,gte,lte}]}, vector:{field,embedding}, vector_weight, aggregations:[{name,type,field,size,interval}], size, from}` → `{hits:[{doc_id,score,source}], total_hits, aggregations, took_millis}`; GET → `{doc_id, found, source}`. Still assumed: refresh ≤ `REFRESH`s; binary names `bin/engine`, `bin/coordinator`; WAL file glob `*.wal`. Adjust `dse.py` adapters only. Needs python3 (stdlib), bash, curl, jq, grpcurl, cmake/clang, go.

## dse.py — client + reference model (shared by all scripts)
```python
import json, urllib.request, urllib.error, re, math, time, os
C = os.environ.get('C', 'http://127.0.0.1:8090'); REFRESH = float(os.environ.get('REFRESH', 2))
def req(m, p, body=None, base=None, timeout=15):
    r = urllib.request.Request((base or C) + p, data=None if body is None else json.dumps(body).encode(), method=m, headers={'Content-Type': 'application/json'})
    try: x = urllib.request.urlopen(r, timeout=timeout); return x.status, json.loads(x.read() or b'null')
    except urllib.error.HTTPError as e: return e.code, (e.read() or b'').decode(errors='replace')
    except Exception as e: return 0, str(e)
create = lambda ix, shards=2, replicas=1: req('PUT', f'/indices/{ix}', {'shards': shards, 'replicas': replicas})
index  = lambda ix, i, d: req('PUT', f'/indices/{ix}/_doc/{i}', d)
get    = lambda ix, i: req('GET', f'/indices/{ix}/_doc/{i}')
delete = lambda ix, i: req('DELETE', f'/indices/{ix}/_doc/{i}')
bulk   = lambda ix, items: req('POST', f'/indices/{ix}/_bulk', {'ops': [{'type': 'index', 'doc_id': i, 'body': d} for i, d in items]}, timeout=120)
def search(ix, text, field='body', size=10000, frm=0, base=None):
    s, r = req('POST', f'/indices/{ix}/_search', {'query': text, 'fields': [field], 'size': size, 'from': frm}, base=base)
    return s, ([(h.get('doc_id'), h.get('score')) for h in (r.get('hits') or [])] if s == 200 else r), (r.get('total_hits') if s == 200 else None)
STOP = set(open(os.environ.get('STOPWORDS', '/dev/null')).read().split()) or {'a','an','and','are','as','at','be','but','by','for','if','in','into','is','it','no','not','of','on','or','such','that','the','their','then','there','these','they','this','to','was','will','with'}  # copy exact list from engine/src/analysis/analyzer.cpp
tok = lambda t: [w for w in (''.join(chr(ord(c)+32) if 'A' <= c <= 'Z' else c for c in t)).split() for w in [w.strip('.,;:!?"()[]{}')] if w and w not in STOP]  # ASCII-only lower + stopwords [src]; tokenizer keeps '-' and "'" inside words
class Model:                                                             # brute-force oracle (I2, I3)
    def __init__(s): s.docs = {}
    def put(s, i, d): s.docs[i] = d
    def rm(s, i): s.docs.pop(i, None)
    def match(s, text, field='body'): q = set(tok(text)); return {i for i, d in s.docs.items() if q & set(tok(d.get(field, '')))}
    def bm25(s, text, field='body', k1=1.2, b=0.75):                     # single-shard / DFS reference; compare ORDER per-shard only if idf is local
        N = len(s.docs); toks = {i: tok(d.get(field, '')) for i, d in s.docs.items()}; avg = (sum(map(len, toks.values())) / N) if N else 0; out = {}
        for t in set(tok(text)):
            df = sum(1 for v in toks.values() if t in v); idf = math.log(1 + (N - df + .5) / (df + .5))
            for i, v in toks.items():
                tf = v.count(t)
                if tf: out[i] = out.get(i, 0) + idf * tf * (k1 + 1) / (tf + k1 * (1 - b + b * len(v) / avg))
        return sorted(out.items(), key=lambda x: (-x[1], x[0]))
WORDS = [f'w{i}' for i in range(300)] + ['alpha', 'beta', 'gamma', 'delta', 'ünïcode', '日本語', 'naïve', 'co-op', "o'neil"]
def rand_doc(r): return {'body': ' '.join(r.choice(WORDS) for _ in range(r.randint(0, 40))), 'n': r.randint(-1000, 1000), 'tag': r.choice(['a', 'b', 'c'])}
def check_search(ix, m, queries, label=''):
    time.sleep(REFRESH); bad = 0
    for qy in queries:
        s, hits, total = search(ix, qy); exp = m.match(qy)
        if s != 200 or {h[0] for h in hits} != exp or (total is not None and total != len(exp)) or len(hits) != len({h[0] for h in hits}):
            bad += 1; got = {h[0] for h in hits} if s == 200 else set(); print(f'FAIL {label} q={qy!r} status={s} missing={sorted(exp-got)[:5]} phantom={sorted(got-exp)[:5]} dup={len(hits)-len(got)} total={total}/{len(exp)}')
        if s == 200 and any(sc is None or sc != sc or sc in (float('inf'),) for _, sc in hits): bad += 1; print('FAIL NaN/Inf score', qy)
    return bad
```

## smoke_functional.py (SMK, FUN-002/004/005/006, NEG-001..006, BND-003/004)
```python
import random, sys, time; from dse import *
ix = f'qa{int(time.time())}'; F = 0
def t(name, cond, info=''):
    global F; print(('PASS ' if cond else 'FAIL ') + name + ('' if cond else f' :: {info}')); F += (not cond)
t('SMK-001', req('GET', '/health')[0] == 200 and req('GET', '/_cluster/health')[0] == 200)
t('FUN-001 create', create(ix)[0] in (200, 201)); t('FUN-001 dup', create(ix)[0] == 409, create(ix))
t('index', index(ix, '1', {'body': 'the quick brown fox'})[0] in (200, 201)); s, d = get(ix, '1'); t('CON-001 realtime get', s == 200 and 'quick' in json.dumps(d), d)
time.sleep(REFRESH); s, h, tot = search(ix, 'quick'); t('SMK-003 search', s == 200 and [x[0] for x in h] == ['1'], h)
index(ix, '1', {'body': 'lazy dog'}); time.sleep(REFRESH); t('update replaces', search(ix, 'quick')[1] == [] and [x[0] for x in search(ix, 'lazy')[1]] == ['1'])
t('delete', delete(ix, '1')[0] in (200, 204)); t('get after delete', get(ix, '1')[0] == 404); time.sleep(REFRESH); t('I2 no phantom', search(ix, 'lazy')[1] == []); t('NEG-003', delete(ix, 'nope')[0] == 404 and get('noindex', '1')[0] == 404)
# model equivalence on random corpus
r = random.Random(7); m = Model(); items = [(f'd{i}', rand_doc(r)) for i in range(5000)]
for i in range(0, 5000, 500): t(f'bulk {i}', bulk(ix, items[i:i + 500])[0] == 200)
[m.put(i, d) for i, d in items]
for i in r.sample(range(5000), 800): delete(ix, f'd{i}'); m.rm(f'd{i}')
for i in r.sample(range(5000), 800): d = rand_doc(r); index(ix, f'd{i}', d); m.put(f'd{i}', d)
qs = ['alpha', 'w1 w2 w3', 'ALPHA', 'ünïcode', '日本語', 'naïve', 'co-op', "o'neil", 'zzznomatch', 'w299 gamma', '']
t('I3 model equivalence', check_search(ix, m, qs, 'I3') == 0)
# ranking on a crafted corpus (single shard to avoid local-idf skew): tf, length norm
ix2 = ix + 'r'; create(ix2, 1, 0); cm = Model()
for i, b in {'tf3': 'cat cat cat dog', 'tf1': 'cat dog bird fish', 'long': 'cat ' + 'filler ' * 50, 'none': 'dog bird'}.items(): index(ix2, i, {'body': b}); cm.put(i, {'body': b})
time.sleep(REFRESH); got = [x[0] for x in search(ix2, 'cat')[1]]; t('FUN-005 BM25 order', got == [x[0] for x in cm.bm25('cat')], f'{got} vs {cm.bm25("cat")}')
# pagination: union of pages == full, no dup
allh = [x[0] for x in search(ix, 'w1 w2 w3 w4 w5', size=10000)[1]]; pages = []
for f in range(0, len(allh) + 50, 50): pages += [x[0] for x in search(ix, 'w1 w2 w3 w4 w5', size=50, frm=f)[1]]
t('FUN-006 pagination', pages == allh, f'{len(pages)} vs {len(allh)}'); t('BND-004', search(ix, 'alpha', size=10, frm=10**6)[0] in (200, 400))
# negative sweep — every case must return 4xx and leave the cluster healthy (I7)
for name in ['', 'UPPER', '..', 'a/b', '_cluster', 'x' * 300, '-lead', 'sp ace', '%2e%2e']: s, _ = req('PUT', f'/indices/{urllib.request.quote(name, safe="")}', {'shards': 1}); t(f'NEG-001 [{name[:12]}]', 400 <= s < 500, s)
for did in ['a/b', '..', 'x' * 2000, 'ü', ' ']: s, _ = index(ix, urllib.request.quote(did, safe=''), {'body': 'x'}); t(f'NEG-002 [{did[:8]}]', s in (200, 201, 400), s)
def raw(m_, p, data): 
    try: return urllib.request.urlopen(urllib.request.Request(C + p, data=data, method=m_, headers={'Content-Type': 'application/json'}), timeout=20).status
    except urllib.error.HTTPError as e: return e.code
    except Exception: return 0
for b in [b'{', b'', b'[]', b'null', b'{"a":NaN}', b'{"n":' + b'9' * 400 + b'}', b'{"body":"\xff\xfe"}', b'{"a":' * 5000 + b'1' + b'}' * 5000, b'{"body":"' + b'x' * 20_000_000 + b'"}']: s = raw('PUT', f'/indices/{ix}/_doc/neg', b); t(f'NEG-004 [{b[:10]!r}]', 400 <= s < 500 or s in (200, 201) and b.startswith(b'{"n"'), s)
for qb in [{'query': {'nope': {}}}, {'query': {'match': {'body': 'a'}}, 'size': -1}, {'query': {'match': {'body': 'a'}}, 'size': 10**9}, {'query': {'match': {'body': 'a'}}, 'from': -5}, {'query': {'bool': {'must': [{'match': {'body': f'w{i}'}} for i in range(20000)]}}}, {'query': {'wildcard': {'body': '*a*' * 200}}}, {}]:
    t0 = time.time(); s, _ = req('POST', f'/indices/{ix}/_search', qb, timeout=30); t(f'NEG-005 {str(qb)[:40]}', s in (200, 400, 413, 422) and time.time() - t0 < 20, s)
t('alive after negatives (I7)', req('GET', '/_cluster/health')[0] == 200 and check_search(ix, m, ['alpha']) == 0)
sys.exit(1 if F else 0)
```

## model_chaos.py — randomized workload + crash/partition injection + full verification (RACE-001, DUR-001/002/006, CHA-001/002/004/009, CON-003)
```python
"""usage: python3 model_chaos.py <seed> <fault: none|kill-engine|kill-coord|pause-engine|flushmerge> ; assumes run_cluster.sh topology, restarts via the same commands."""
import random, sys, subprocess, threading, time, signal, os; from dse import *
seed, fault = int(sys.argv[1]), sys.argv[2]; r = random.Random(seed); ix = f'chaos{seed}{int(time.time())}'; create(ix, 2, 1); m = Model(); maybe = {}; lock = threading.Lock(); stop = False
ROOT = os.environ.get('ROOT', '..'); DATA = os.environ.get('DATA_DIR', f'{ROOT}/data')
ENG = lambda n: [f'{ROOT}/bin/engine', f'--port=700{n}', f'--data-dir={DATA}/engine{n}', f'--node-id=engine-{n}', '--maintenance-interval-sec=1']
COORD = [f'{ROOT}/bin/coordinator', '-node-id=coord1', '-http-addr=127.0.0.1:8090', '-raft-addr=127.0.0.1:9010', f'-raft-dir={DATA}/coord1/raft', '-engine-nodes=localhost:7001,localhost:7002']
pid = lambda pat: subprocess.run(['pgrep', '-f', pat], capture_output=True, text=True).stdout.split()
def writer(w):
    rr = random.Random(seed * 1000 + w)
    while not stop:
        i = f'k{w}-{rr.randrange(300)}'                          # ids private per writer => exact expectation, no cross-writer ambiguity
        if rr.random() < 0.75:
            d = rand_doc(rr); s, _ = index(ix, i, d)
            with lock:
                if s in (200, 201): m.put(i, d); maybe.pop(i, None)
                else: maybe.setdefault(i, []).append(d)          # outcome unknown: old state or this doc are both legal
        else:
            s, _ = delete(ix, i)
            with lock:
                if s in (200, 204, 404): m.rm(i); maybe.pop(i, None)
                else: maybe.setdefault(i, []).append(None)
def searcher():
    while not stop:
        s, hits, _ = search(ix, r.choice(WORDS))
        if s == 200 and len(hits) != len({h[0] for h in hits}): print('FAIL RACE-004 duplicate hit (same doc from two segments/copies)'); os._exit(1)
ths = [threading.Thread(target=writer, args=(w,)) for w in range(16)] + [threading.Thread(target=searcher) for _ in range(8)]; [t.start() for t in ths]
for cycle in range(int(os.environ.get('CYCLES', 10))):
    time.sleep(r.uniform(3, 8))
    if fault == 'kill-engine':  n = r.choice([1, 2]); [os.kill(int(p), signal.SIGKILL) for p in pid(f'port=700{n}')]; time.sleep(r.uniform(1, 5)); subprocess.Popen(ENG(n))
    if fault == 'kill-coord':   [os.kill(int(p), signal.SIGKILL) for p in pid('http-addr=127.0.0.1:8090')]; time.sleep(2); subprocess.Popen(COORD)
    if fault == 'pause-engine': ps = pid('port=7001'); [os.kill(int(p), signal.SIGSTOP) for p in ps]; time.sleep(10); [os.kill(int(p), signal.SIGCONT) for p in ps]
    if fault == 'flushmerge':   [subprocess.run(['grpcurl', '-plaintext', '-d', '{}', f'localhost:700{n}', f'dse.EngineService/{rpc}'], capture_output=True) for n in (1, 2) for rpc in ('Flush', 'Merge')]   # service/pkg name: VERIFY in proto
stop = True; [t.join() for t in ths]; time.sleep(15)
bad = 0
for i in set(m.docs) | set(maybe):                                # per-document verification via realtime GET
    s, d = get(ix, i); got = (d.get('source') if isinstance(d, dict) and d.get('found') else None) if s == 200 else None; legal = [m.docs.get(i)] + maybe.get(i, [])
    if not any((got is None and L is None) or (got is not None and L is not None and got.get('body') == L.get('body')) for L in legal): bad += 1; print('FAIL DUR/I2', i, 'status', s)
strict = Model(); [strict.put(i, d) for i, d in m.docs.items() if i not in maybe]          # search check restricted to docs with certain state
time.sleep(REFRESH)
for qy in WORDS[:40]:
    s, hits, _ = search(ix, qy); got = {h[0] for h in hits} if s == 200 else set(); exp = strict.match(qy); unsure = set(maybe)
    if s != 200 or (exp - got) or ((got - exp) - unsure) or len(hits) != len(got): bad += 1; print(f'FAIL I3 q={qy} status={s} missing={sorted(exp-got)[:3]} phantom={sorted((got-exp)-unsure)[:3]}')
print(f'seed={seed} fault={fault} docs={len(m.docs)} uncertain={len(maybe)} failures={bad}'); sys.exit(1 if bad else 0)
```
Run matrix: `for f in none flushmerge kill-engine kill-coord pause-engine; do for s in $(seq 1 20); do python3 model_chaos.py $s $f || break 2; done; done`.

## replica_diff.sh (CON-003, I4) — after any chaos run
```bash
#!/usr/bin/env bash
# Per shard, compare doc counts/ids between copies using ShardStatus (and a match_all search per engine if the engine exposes per-shard search).
for n in 1 2; do grpcurl -plaintext -d '{}' localhost:700$n dse.EngineService/ShardStatus | jq -c '{node:"engine-'$n'", shards:[.shards[]? | {id:.shardId, docs:.docCount, deleted:.deletedCount, segments:.segmentCount}]}'; done | tee /tmp/shards.json
jq -s '[.[].shards[]] | group_by(.id) | map(select((map(.docs)|unique|length)>1)) | if length==0 then "PASS CON-003 replicas agree" else {FAIL_divergent_shards:.} end' /tmp/shards.json
```

## wal_segment_corruption.sh (DUR-003/005, I7)
```bash
#!/usr/bin/env bash
set -u; ROOT=${ROOT:-..}; D=${DATA_DIR:-$ROOT/data}; C=http://127.0.0.1:8090
$ROOT/scripts/stop_cluster.sh; rm -rf $D; $ROOT/scripts/run_cluster.sh; sleep 3; curl -s -X PUT $C/indices/dur -d '{"shards":1,"replicas":0}' >/dev/null
for i in $(seq 500); do curl -s -o /dev/null -X PUT $C/indices/dur/_doc/$i -H 'Content-Type: application/json' -d "{\"body\":\"doc$i common\"}"; done
$ROOT/scripts/stop_cluster.sh; cp -r $D /tmp/dse-golden; WAL=$(find $D -name '*.wal' -o -name 'wal*' -type f | head -1); SZ=$(wc -c < "$WAL"); echo "wal=$WAL size=$SZ"
for cut in $(seq 1 64); do rm -rf $D; cp -r /tmp/dse-golden $D; truncate -s $((SZ-cut)) "$WAL"; $ROOT/scripts/run_cluster.sh --keep-data >/dev/null 2>&1; sleep 3
  pgrep -f 'port=700' >/dev/null || { echo "FAIL DUR-003 cut=$cut engine crashed on torn WAL"; continue; }
  n=$(curl -s -X POST $C/indices/dur/_search -H 'Content-Type: application/json' -d '{"query":"common","fields":["body"],"size":1000}' | jq '.total_hits'); [[ $n -ge 498 && $n -le 500 ]] || echo "FAIL DUR-003 cut=$cut total=$n"
  [[ $(curl -s -o /dev/null -w '%{http_code}' $C/indices/dur/_doc/1) == 200 ]] || echo "FAIL DUR-003 cut=$cut early doc lost"; $ROOT/scripts/stop_cluster.sh >/dev/null 2>&1; done
for f in $(find /tmp/dse-golden -type f ! -name '*.lock' | head -40); do for off in 0 17 $(( $(wc -c < $f) / 2 )); do rm -rf $D; cp -r /tmp/dse-golden $D; t=$D/${f#/tmp/dse-golden/}; printf '\xff\x00\xff' | dd of="$t" bs=1 seek=$off conv=notrunc 2>/dev/null
  $ROOT/scripts/run_cluster.sh --keep-data >/tmp/run.log 2>&1; sleep 3; curl -s -m 10 -X POST $C/indices/dur/_search -H 'Content-Type: application/json' -d '{"query":"common","fields":["body"],"size":1000}' > /tmp/out.json
  grep -qiE 'segmentation|core dumped|AddressSanitizer|SIGABRT' /tmp/run.log $D/*.log 2>/dev/null && echo "FAIL DUR-005 crash: file=$(basename $f) off=$off"
  jq -e '[.hits[]? | (.source.body // "")] | all(test("^doc[0-9]+ common$"))' /tmp/out.json >/dev/null || echo "FAIL DUR-005 garbage served: file=$(basename $f) off=$off"; $ROOT/scripts/stop_cluster.sh >/dev/null 2>&1; done; done
echo done
```

## load.py (LOAD-001..003, STR-001, SPK-001, SOAK-001) — stdlib, closed-loop workers
```python
import sys, time, random, threading, statistics; from dse import *
profile, ix = sys.argv[1], 'load'; create(ix, 4, 1); lat = {'bulk': [], 'search': []}; err = [0]; L = threading.Lock(); r0 = random.Random(1)
def bulk_w(n):
    r = random.Random(n)
    for b in range(125): t = time.perf_counter(); s, _ = bulk(ix, [(f'{n}-{b}-{i}', rand_doc(r)) for i in range(1000)]); dt = time.perf_counter() - t; (lat['bulk'].append(dt) if s == 200 else err.__setitem__(0, err[0] + 1))
def search_w(stop_at, think):
    r = random.Random(threading.get_ident())
    while time.time() < stop_at:
        q = ' '.join(r.choice(WORDS) for _ in range(r.choice([1, 1, 2, 3]))); t = time.perf_counter(); s, _, _ = search(ix, q, size=10); dt = time.perf_counter() - t
        with L: (lat['search'].append(dt) if s == 200 else err.__setitem__(0, err[0] + 1))
        time.sleep(think)
t0 = time.time(); ws = [threading.Thread(target=bulk_w, args=(n,)) for n in range(8)]; [w.start() for w in ws]; [w.join() for w in ws]; print(f'LOAD-001 indexed 1M docs in {time.time()-t0:.0f}s => {1e6/(time.time()-t0):.0f} docs/s errors={err[0]}')
stages = {'load': [(64, 600)], 'stress': [(64, 120), (256, 180), (1024, 180), (16, 120)], 'spike': [(8, 60), (800, 30), (8, 60), (800, 30), (8, 60)], 'soak': [(32, 86400)]}[profile]
for n, dur in stages:
    lat['search'].clear(); e0 = err[0]; end = time.time() + dur; ts = [threading.Thread(target=search_w, args=(end, 0.05)) for _ in range(n)]; [t.start() for t in ts]; [t.join() for t in ts]
    q = statistics.quantiles(lat['search'], n=100) if len(lat['search']) > 100 else [0] * 99; print(f'{profile} clients={n} dur={dur}s qps={len(lat["search"])/dur:.0f} p50={q[49]*1000:.1f}ms p99={q[98]*1000:.1f}ms errors={err[0]-e0}')
s, _ = req('GET', '/_cluster/health'); print('PASS alive' if s == 200 else 'FAIL cluster dead after load'); sys.exit(0 if s == 200 else 1)
```
During soak sample every 60 s: `ps -o rss= -p $(pgrep -f port=7001)`, `lsof -p … | wc -l`, `du -sk data/`; fail on monotonic growth after warm-up.

## Sanitizer / fuzz builds (RACE-009, security)
```bash
cd engine
cmake -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" && cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure
cmake -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fsanitize=thread" && cmake --build build-tsan -j && ctest --test-dir build-tsan
# then run the cluster with the asan/tsan engine binary under smoke_functional.py + model_chaos.py (fault=none, flushmerge)
cd ../coordinator && go test -race ./... && go test -fuzz=FuzzSearchRequest -fuzztime=10m ./internal/api
```
```cpp
// engine/fuzz/fuzz_wal.cpp  (clang++ -fsanitize=fuzzer,address,undefined) — same pattern for segment reader, bitmap deserialize, analyzer
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { auto dir = TempDir(); write_file(dir / "0.wal", d, n); try { dse::Wal w(dir); w.replay([](const dse::WalRecord&){}); } catch (const std::exception&) {} return 0; }
// engine/tests/bitmap_boundary_test.cpp (BND-001): ids {0,65535,65536,2^31-1,2^32-1}; containers with 4095/4096/4097 members; AND/OR/ANDNOT vs std::set reference over 10k random seeded sets
// engine/tests/segment_merge_model_test.cpp (DUR-004/006): random adds/deletes across N segments -> merge -> live set == reference; deleted never present; with failpoints at each merge stage then reopen
// engine/tests/scoring_extremes_test.cpp (BND-003): N=1, df=N, empty field, avgdl=0 -> std::isfinite(score)
```
```go
// coordinator/internal/routing/property_test.go: same id -> same shard across 1e5 calls & process restarts (golden hashes); χ² uniformity over 1e5 ids for shard counts 1..16
// coordinator/internal/cluster/fsm_test.go: apply random command log -> snapshot -> restore -> state equal; 3-node in-memory raft: leader kill -> committed index metadata present on new leader
// coordinator/internal/api/merge_test.go: top-k merge of per-shard hit lists == global sort (score desc, id asc) for random inputs incl. ties, empty shards, shard errors -> partial flag
```

## Run order
ctest + `go test -race` → ASan/TSan unit → smoke_functional → model_chaos (none, flushmerge) → load → model_chaos (kill-engine, kill-coord, pause-engine) + replica_diff → wal_segment_corruption → fuzzers → stress/spike → soak.
