#!/usr/bin/env python3
# Author：FDE WorkMate
"""End-to-end HNSW build benchmark for seekDB.

Loads vectors into a table, times CREATE VECTOR INDEX, then measures recall@k
against brute-force ground truth computed with numpy. Run it once against an
observer without SEEKDB_HIPVS_ENABLE and once with it, then compare.

  hnsw_build_bench.py load  --port P --rows N --metric l2 [--hdf5 sift.hdf5]
  hnsw_build_bench.py build --port P --metric l2
  hnsw_build_bench.py query --port P --rows N --metric l2 [--hdf5 ...]
  hnsw_build_bench.py dml   --port P --metric l2
"""

import argparse
import json
import sys
import time

import numpy as np
import pymysql

TABLE = "hnsw_bench"
INDEX = "hnsw_bench_idx"
DISTANCE_SQL = {"l2": ("l2", "l2_distance"), "ip": ("inner_product", "negative_inner_product")}


def connect(port):
    return pymysql.connect(host="127.0.0.1", port=port, user="root", password="", database="test",
                           autocommit=True, read_timeout=36000)


def dataset(args):
    if args.hdf5:
        import h5py
        with h5py.File(args.hdf5, "r") as f:
            base = np.asarray(f["train"][: args.rows], dtype=np.float32)
            queries = np.asarray(f["test"][: args.queries], dtype=np.float32)
    else:
        # Reproducible clustered data (Gaussian mixture) as a stand-in for embeddings.
        rng = np.random.default_rng(20261028)
        centers = rng.standard_normal((1000, args.dim), dtype=np.float32) * 4
        def sample(n):
            return (centers[rng.integers(0, len(centers), n)] +
                    rng.standard_normal((n, args.dim), dtype=np.float32)).astype(np.float32)
        base = sample(args.rows)
        queries = sample(args.queries)
    if args.metric == "ip":
        # bounded norms keep inner-product neighbourhoods meaningful
        base /= np.linalg.norm(base, axis=1, keepdims=True)
        queries /= np.linalg.norm(queries, axis=1, keepdims=True)
    return base, queries


def literal(vector):
    return "[" + ",".join("%.6g" % x for x in vector) + "]"


def ground_truth(base, queries, metric, k):
    if metric == "l2":
        scores = (queries ** 2).sum(1)[:, None] - 2 * queries @ base.T + (base ** 2).sum(1)[None, :]
    else:
        scores = -(queries @ base.T)
    return np.argsort(scores, axis=1)[:, :k]


def cmd_load(args):
    base, _ = dataset(args)
    conn = connect(args.port)
    cur = conn.cursor()
    cur.execute("DROP TABLE IF EXISTS %s" % TABLE)
    cur.execute("CREATE TABLE %s (id BIGINT PRIMARY KEY, v VECTOR(%d))" % (TABLE, base.shape[1]))
    started = time.time()
    for offset in range(0, len(base), 2000):
        rows = [(offset + i, literal(v)) for i, v in enumerate(base[offset: offset + 2000])]
        cur.executemany("INSERT INTO %s VALUES (%%s, %%s)" % TABLE, rows)
    print(json.dumps({"step": "load", "rows": len(base), "dim": base.shape[1],
                      "seconds": round(time.time() - started, 2)}))


def cmd_build(args):
    distance, _ = DISTANCE_SQL[args.metric]
    conn = connect(args.port)
    cur = conn.cursor()
    cur.execute("DROP INDEX %s ON %s" % (INDEX, TABLE)) if args.drop else None
    started = time.time()
    cur.execute("CREATE VECTOR INDEX %s ON %s(v) WITH (distance=%s, type=hnsw, lib=vsag, m=%d, "
                "ef_construction=%d)" % (INDEX, TABLE, distance, args.m, args.ef_construction))
    print(json.dumps({"step": "build", "metric": args.metric, "m": args.m,
                      "ef_construction": args.ef_construction,
                      "seconds": round(time.time() - started, 2)}))


def search(cur, function, query, k):
    cur.execute("SELECT id FROM %s ORDER BY %s(v, '%s') APPROXIMATE LIMIT %d"
                % (TABLE, function, literal(query), k))
    return [row[0] for row in cur.fetchall()]


def cmd_query(args):
    base, queries = dataset(args)
    _, function = DISTANCE_SQL[args.metric]
    truth = ground_truth(base, queries, args.metric, args.k)
    conn = connect(args.port)
    cur = conn.cursor()
    result = {"step": "query", "metric": args.metric, "k": args.k, "queries": len(queries), "recall": {}}
    for ef in args.ef_search:
        cur.execute("SET ob_hnsw_ef_search = %d" % ef)
        hits = 0
        started = time.time()
        for qi, query in enumerate(queries):
            hits += len(set(search(cur, function, query, args.k)) & set(truth[qi].tolist()))
        result["recall"][str(ef)] = {"recall": round(hits / (len(queries) * args.k), 4),
                                     "avg_ms": round((time.time() - started) * 1000 / len(queries), 2)}
    print(json.dumps(result))


def cmd_dml(args):
    """Index must keep working after import: insert, find, delete, not find."""
    _, function = DISTANCE_SQL[args.metric]
    conn = connect(args.port)
    cur = conn.cursor()
    cur.execute("SELECT vector_dims(v) FROM %s LIMIT 1" % TABLE)
    dim = cur.fetchone()[0]
    # A far-away, large-norm probe is its own nearest neighbour for both l2 and ip.
    probe = np.full(dim, 777.0, dtype=np.float32)
    new_id = 10 ** 12
    cur.execute("INSERT INTO %s VALUES (%d, '%s')" % (TABLE, new_id, literal(probe)))
    found = new_id in search(cur, function, probe, 10)
    cur.execute("DELETE FROM %s WHERE id = %d" % (TABLE, new_id))
    gone = new_id not in search(cur, function, probe, 10)
    print(json.dumps({"step": "dml", "insert_visible": found, "delete_visible": gone}))
    return 0 if found and gone else 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["load", "build", "query", "dml"])
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--rows", type=int, default=100000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--queries", type=int, default=200)
    parser.add_argument("--metric", choices=["l2", "ip"], default="l2")
    parser.add_argument("--hdf5")
    parser.add_argument("--m", type=int, default=16)
    parser.add_argument("--ef_construction", type=int, default=200)
    parser.add_argument("--ef_search", type=int, nargs="+", default=[64, 128, 256])
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--drop", action="store_true")
    args = parser.parse_args()
    return {"load": cmd_load, "build": cmd_build, "query": cmd_query, "dml": cmd_dml}[args.command](args) or 0


if __name__ == "__main__":
    sys.exit(main())
