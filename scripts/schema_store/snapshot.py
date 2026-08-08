#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
snapshot.py — 把**真實的 34 個套件**縮成一份可以進版控的測試語料。

為什麼要這個東西
─────────────────────────────────────────────────────────────────────────────
撞號偵測要有意義，就必須拿真資料跑：假資料只會證明「我寫的偵測器認得我自己
編的例子」。但真資料是 891MB 的上游 clone，不可能進版控，CI 上也不會有。

折衷：把**判定需要的那幾個欄位**抽出來（套件 id、方案 id 與名稱、zip 會寫出
哪些路徑、requires、rppi 路徑，以及那一次跑出來的語言判定結果），存成
`data/corpus.json`。它就是那 34 個套件，只是脫水了。

    · 測試永遠跑得動（不需要 clone、不需要模擬器）
    · 語料一旦與上游脫節，`test_corpus_matches_live`（有真資料時才跑）會紅

⚠ 這份語料是**快照，不是真相**。真相在上游。所以：
    1. 每次重新 build 市集之後要跑一次這支重新產生；
    2. test_store.py 有一條測試專門比對快照與現場，現場不在時它會**大聲說
       自己沒跑**（而不是安靜地綠）。

用法
    python3 snapshot.py <WORK> [--out data/corpus.json]
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import languages   # noqa: E402
import uid as uidlib   # noqa: E402

DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
DEFAULT_OUT = os.path.join(DATA_DIR, "corpus.json")

SNAPSHOT_FORMAT = 1


def build_corpus(work, with_languages=True):
    doc = json.load(open(os.path.join(work, "packages.json")))
    keep = set(doc.get("index_ids") or [p["id"] for p in doc["packages"]])
    verify_dir = os.path.join(work, "verify")

    schemas_ok = {}
    for p in doc["packages"]:
        fp = os.path.join(verify_dir, p["id"] + ".json")
        if os.path.exists(fp):
            schemas_ok[p["id"]] = set(json.load(open(fp)).get("schemas_ok") or [])
        else:
            schemas_ok[p["id"]] = set()

    packages = []
    for p in sorted(doc["packages"], key=lambda x: x["id"]):
        if p["id"] not in keep:
            continue
        ok = schemas_ok.get(p["id"]) or set()
        packages.append({
            "id": p["id"],
            "rppi_path": p.get("rppi_path"),
            "requires": sorted(p.get("requires") or []),
            "schemas": [{"id": s["id"], "name": s.get("name") or ""}
                        for s in p["schemas"] if s["id"] in ok],
            # zip 解壓後會落在 user_data_dir 的相對路徑。撞名偵測看的就是這個。
            "entries": uidlib.package_entries(p),
        })

    out = {
        "snapshot_format": SNAPSHOT_FORMAT,
        "generated_at": datetime.datetime.now(datetime.timezone.utc)
                        .replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "source": "build/schema-store/_work/packages.json",
        "package_count": len(packages),
        "packages": packages,
    }

    if with_languages:
        data = languages.load_data()
        _, rows, builtin = languages.build(work, only_pkgs=keep,
                                           schemas_of=schemas_ok, data=data)
        out["builtin"] = [{"uid": b["uid"], "id": b["id"], "name": b["name"],
                           "language": b["language"], "source": b["source"],
                           "rule": b["rule"],
                           "not_a_language": b.get("not_a_language", False)}
                          for b in builtin]
        out["languages"] = {r["uid"]: {"language": r["language"],
                                       "source": r["source"], "rule": r["rule"],
                                       "not_a_language": r.get("not_a_language", False)}
                            for r in rows}
        out["coverage"] = languages.coverage(rows, builtin)
        out["coverage"].pop("unknown_items", None)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="產生測試用的真實語料快照")
    ap.add_argument("work", help="build/schema-store/_work")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--no-languages", action="store_true",
                    help="不跑語言判定（沒有上游 clone 時）")
    args = ap.parse_args(argv)

    corpus = build_corpus(args.work, with_languages=not args.no_languages)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(corpus, f, ensure_ascii=False, indent=1, sort_keys=False)
        f.write("\n")
    nsch = sum(len(p["schemas"]) for p in corpus["packages"])
    nent = sum(len(p["entries"]) for p in corpus["packages"])
    print("語料已寫入 %s：%d 個套件、%d 個方案、%d 個 zip entry"
          % (args.out, corpus["package_count"], nsch, nent))
    if "coverage" in corpus:
        c = corpus["coverage"]
        print("  語言標記：有標記 %d / 不是語言 %d / 未知 %d（涵蓋率 %.1f%%）"
              % (c["tagged"], c["not_a_language"], c["unknown"], c["coverage_pct"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
