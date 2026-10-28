#!/usr/bin/env python3
# seekdb-agent-memory: persistent agent memory on embedded seekdb (pyseekdb).
# 作者：FDE WorkMate
#
# Subcommands: add, search, hybrid, fork, drop, list, selftest.
# Every command prints a single JSON object to stdout so an agent can parse it.
# Embeddings are computed client-side and passed explicitly, so the collection
# dimension is always checked against the chosen embedder.

import argparse
import hashlib
import json
import math
import re
import sys
import tempfile

try:
    import pyseekdb
except ImportError:  # pragma: no cover - environment guard
    print(json.dumps({"ok": False, "error": "pyseekdb not installed: pip install -U pyseekdb"}))
    sys.exit(2)


# ---------------------------------------------------------------- embedders --

class HashEmbedder:
    """Deterministic, offline bag-of-tokens embedder (feature hashing).

    Useful for CI, air-gapped machines and smoke tests. It captures lexical
    overlap only; use the default model for semantic recall.
    """

    dimension = 256
    _token = re.compile(r"[\w\u4e00-\u9fff]+", re.UNICODE)

    def __call__(self, texts):
        out = []
        for text in texts:
            vec = [0.0] * self.dimension
            for tok in self._token.findall(text.lower()):
                h = int(hashlib.md5(tok.encode("utf-8")).hexdigest(), 16)
                vec[h % self.dimension] += 1.0 if (h >> 8) & 1 else -1.0
            norm = math.sqrt(sum(v * v for v in vec)) or 1.0
            out.append([v / norm for v in vec])
        return out


class DefaultEmbedder:
    """pyseekdb built-in all-MiniLM-L6-v2 (ONNX, 384 dims).

    First call downloads ~86 MB into ~/.cache/pyseekdb/onnx_models from
    HF_ENDPOINT (pyseekdb defaults to https://hf-mirror.com).
    """

    dimension = 384

    def __init__(self):
        self._ef = pyseekdb.DefaultEmbeddingFunction()

    def __call__(self, texts):
        return [list(map(float, v)) for v in self._ef(list(texts))]


EMBEDDERS = {"default": DefaultEmbedder, "hash": HashEmbedder}


# ------------------------------------------------------------------ helpers --

def parse_kv(pairs):
    """Turn ["k=v", "n=3"] into {"k": "v", "n": 3}; ints/floats/bools are typed."""
    result = {}
    for pair in pairs or []:
        if "=" not in pair:
            raise ValueError(f"expected key=value, got {pair!r}")
        key, raw = pair.split("=", 1)
        value = raw
        if raw.lower() in ("true", "false"):
            value = raw.lower() == "true"
        else:
            for cast in (int, float):
                try:
                    value = cast(raw)
                    break
                except ValueError:
                    continue
        result[key] = value
    return result


def build_where(pairs):
    """key=value pairs -> pyseekdb metadata filter (AND of $eq)."""
    meta = parse_kv(pairs)
    if not meta:
        return None
    clauses = [{k: {"$eq": v}} for k, v in meta.items()]
    return clauses[0] if len(clauses) == 1 else {"$and": clauses}


def open_collection(args, embedder):
    client = pyseekdb.Client(path=args.db)
    col = client.get_or_create_collection(
        name=args.collection,
        embedding_function=None,
        configuration=pyseekdb.HNSWConfiguration(dimension=embedder.dimension, distance="cosine"),
    )
    if col.dimension != embedder.dimension:
        raise ValueError(
            f"collection {args.collection!r} has dimension {col.dimension}, "
            f"embedder {args.embedder!r} produces {embedder.dimension}; use the embedder the collection was created with"
        )
    return client, col


def flatten(result):
    """Convert pyseekdb's nested single-query result into a list of hits."""
    ids = result.get("ids", [[]])[0]
    keys = [k for k in ("distances", "documents", "metadatas") if result.get(k)]
    hits = []
    for i, _id in enumerate(ids):
        hit = {"id": _id}
        for k in keys:
            hit[k[:-1]] = result[k][0][i]
        hits.append(hit)
    return hits


# ----------------------------------------------------------------- commands --

def cmd_add(args, embedder):
    _, col = open_collection(args, embedder)
    texts = args.text
    ids = args.id or []
    if len(ids) != len(texts):
        raise ValueError("--id and --text must be given the same number of times")
    meta = parse_kv(args.meta)
    col.upsert(ids=ids, documents=texts, embeddings=embedder(texts), metadatas=[dict(meta) for _ in texts])
    if not args.no_refresh:
        # Vector index build is asynchronous; refresh gives read-your-writes.
        col.refresh_index()
    return {"ok": True, "upserted": ids, "count": col.count(), "refreshed": not args.no_refresh}


def cmd_search(args, embedder):
    _, col = open_collection(args, embedder)
    res = col.query(
        query_embeddings=embedder([args.query])[0],
        n_results=args.n,
        where=build_where(args.where),
        include=["documents", "metadatas"],
    )
    return {"ok": True, "metric": "cosine_distance (lower is closer)", "hits": flatten(res)}


def cmd_hybrid(args, embedder):
    _, col = open_collection(args, embedder)
    where = build_where(args.where)
    text_part = {"where_document": {"$contains": args.keyword or args.query}, "n_results": args.window}
    knn_part = {"query_embeddings": embedder([args.query]), "n_results": args.window}
    if where:
        text_part["where"] = where
        knn_part["where"] = where
    res = col.hybrid_search(
        query=text_part, knn=knn_part, rank={"rrf": {}}, n_results=args.n, include=["documents", "metadatas"]
    )
    hits = flatten(res)
    for hit in hits:  # hybrid "distances" are RRF scores: higher is better
        hit["rrf_score"] = hit.pop("distance", None)
    return {"ok": True, "metric": "rrf_score (higher is better)", "hits": hits}


def cmd_fork(args, embedder):
    _, col = open_collection(args, embedder)
    forked = col.fork(args.to)
    return {"ok": True, "source": col.name, "fork": forked.name, "count": forked.count()}


def cmd_drop(args, embedder):
    client = pyseekdb.Client(path=args.db)
    client.delete_collection(args.collection)
    return {"ok": True, "dropped": args.collection}


def cmd_list(args, embedder):
    client = pyseekdb.Client(path=args.db)
    return {"ok": True, "collections": [{"name": c.name, "count": c.count()} for c in client.list_collections()]}


def cmd_selftest(args, embedder):
    """End-to-end check in a throw-away database; exits non-zero on any failure."""
    checks = []
    with tempfile.TemporaryDirectory() as tmp:
        ns = argparse.Namespace(db=f"{tmp}/selftest.db", collection="mem", embedder=args.embedder)
        _, col = open_collection(ns, embedder)
        docs = {
            "m1": ("user prefers dark mode in the IDE", {"kind": "pref"}),
            "m2": ("deploy failed: port 8080 already in use", {"kind": "error"}),
            "m3": ("user wants answers in Chinese", {"kind": "pref"}),
        }
        texts = [d[0] for d in docs.values()]
        col.upsert(ids=list(docs), documents=texts, embeddings=embedder(texts), metadatas=[d[1] for d in docs.values()])
        col.refresh_index()

        top = flatten(col.query(query_embeddings=embedder(["dark mode IDE"])[0], n_results=1))
        checks.append(("read_your_writes_after_refresh", bool(top) and top[0]["id"] == "m1"))

        filt = flatten(col.query(query_embeddings=embedder(["port"])[0], n_results=3, where={"kind": {"$eq": "pref"}}))
        checks.append(("metadata_filter", {h["id"] for h in filt} <= {"m1", "m3"} and len(filt) > 0))

        hyb = flatten(col.hybrid_search(
            query={"where_document": {"$contains": "8080"}, "n_results": 5},
            knn={"query_embeddings": embedder(["deploy failed"]), "n_results": 5},
            rank={"rrf": {}}, n_results=1,
        ))
        checks.append(("hybrid_rrf", bool(hyb) and hyb[0]["id"] == "m2"))

        sandbox = col.fork("mem_sandbox")
        sandbox.delete(ids=["m1"])
        checks.append(("fork_isolation", sandbox.count() == 2 and col.count() == 3))

        reopened = pyseekdb.Client(path=ns.db).get_collection("mem", embedding_function=None)
        checks.append(("persist_reopen", reopened.count() == 3))

    report = {name: passed for name, passed in checks}
    return {"ok": all(report.values()), "embedder": args.embedder, "checks": report}


# --------------------------------------------------------------------- main --

def main(argv=None):
    p = argparse.ArgumentParser(description="Agent memory on embedded seekdb")
    p.add_argument("--db", default="./agent_memory.db", help="seekdb data directory (created if missing)")
    p.add_argument("--collection", default="memory", help="collection name (letters, digits, underscore)")
    p.add_argument("--embedder", choices=sorted(EMBEDDERS), default="default")
    sub = p.add_subparsers(dest="cmd", required=True)

    a = sub.add_parser("add", help="upsert memories (idempotent by id)")
    a.add_argument("--id", action="append", required=True)
    a.add_argument("--text", action="append", required=True)
    a.add_argument("--meta", action="append", help="key=value, applied to every item")
    a.add_argument("--no-refresh", action="store_true", help="skip index refresh (bulk loads)")

    for name in ("search", "hybrid"):
        s = sub.add_parser(name)
        s.add_argument("--query", required=True)
        s.add_argument("-n", type=int, default=5)
        s.add_argument("--where", action="append", help="metadata key=value filter, ANDed")
        if name == "hybrid":
            s.add_argument("--keyword", help="full-text term; defaults to --query")
            s.add_argument("--window", type=int, default=20, help="candidates per branch before RRF")

    f = sub.add_parser("fork", help="copy-on-write sandbox of the collection")
    f.add_argument("--to", required=True)
    sub.add_parser("drop", help="delete the collection named by --collection")
    sub.add_parser("list")
    sub.add_parser("selftest")

    args = p.parse_args(argv)
    handlers = {
        "add": cmd_add, "search": cmd_search, "hybrid": cmd_hybrid, "fork": cmd_fork,
        "drop": cmd_drop, "list": cmd_list, "selftest": cmd_selftest,
    }
    try:
        embedder = EMBEDDERS[args.embedder]() if args.cmd not in ("drop", "list") else None
        result = handlers[args.cmd](args, embedder)
    except Exception as exc:  # report as JSON so the calling agent can react
        result = {"ok": False, "error": f"{type(exc).__name__}: {exc}"}
    print(json.dumps(result, ensure_ascii=False))
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
