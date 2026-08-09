#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
registry.py — 安裝紀錄的 v1 → v2 遷移**參考實作**。

帳本的檔名由 `scripts/lib/product.env` 的 `STORE_REGISTRY_FILE` 決定,
2026-08-09 隨產品改名換成以新識別碼字根開頭的那個名字。
⚠ **換檔名本身就是一次遷移**:舊名字的檔案還在既有使用者的裝置上,讀取端必須
兩個名字都認得,否則升級之後「裝過哪些方案」會整批消失,而且不會有錯誤訊息 ——
畫面上就只是一個乾淨的空清單。

這支不在 app 裡跑。它存在的理由有兩個：
  1. 讓遷移演算法有一份可執行、可測試、可以拿去對照的定義 ——
     Android 端照著實作，並用 `data/registry_v1_sample.json` 與這裡算出來的
     結果比對，就不會兩邊各寫一套。
  2. 使用者手上**已經裝了東西**。遷移寫錯的代價是「升級之後方案不見了」，
     那比一開始就沒有 uid 更糟。

為什麼遷移是無損的
─────────────────────────────────────────────────────────────────────────────
v1 的帳本本來就是**以套件為單位**存的：頂層每一筆是一個套件，方案掛在它下面。
也就是說「這個方案是哪個套件裝的」這個資訊 v1 已經有了，只是沒有把它寫成一個
可以當鍵用的字串。所以遷移就是：

    uid = <套件 id>/<方案 id>        （source == "store"）
    uid = @local/<清理過的套件 id>/<方案 id>   （source == "local"）

不需要向伺服器問任何事、不需要網路（這很重要：本專案離線為預設）、
也不會有「猜錯」的可能。

⚠ 遷移**修不好**的一件事
─────────────────────────────────────────────────────────────────────────────
如果使用者先裝 double-pinyin 再裝 ice，`double_pinyin.schema.yaml` 早就被
後者蓋掉了。帳本兩筆都在，但磁碟上只剩一份，而帳本記不得是誰的。
遷移只能**指出**這件事（回傳 `file_conflicts`），不能還原 ——
要還原只能重新解壓其中一個套件。這是 uid 出現得太晚的既成代價，
誠實寫出來比假裝已經修好要好。
"""

from __future__ import annotations

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, "lib"))
import uid as uidlib   # noqa: E402
import product         # noqa: E402

FORMAT_V1 = 1
FORMAT_V2 = 2


def schema_uid_for(pkg, schema_id):
    """一筆帳本紀錄裡的某個方案 → uid。"""
    pid = pkg.get("id") or ""
    if (pkg.get("source") or "store") == "local":
        # Android 目前給自帶檔案的 id 是 "local:" + 檔名主幹，含 `:`，
        # 而且是使用者輸入衍生的。local_uid() 會把不合法的字元換成 `_`。
        origin = pid[len("local:"):] if pid.startswith("local:") else pid
        return uidlib.local_uid(origin, schema_id)
    return uidlib.make_uid(pid, schema_id)


def migrate(doc):
    """v1（或已是 v2）的帳本 dict → (新的 dict, 報告)。

    冪等：v2 進去、同樣的 v2 出來。這一點有測試釘住 —— 遷移程式最常見的
    壞法就是「跑第二次把資料弄壞」。
    """
    report = {"from": doc.get("format_version"), "to": FORMAT_V2,
              "packages": 0, "schemas": 0, "added_uid": 0,
              "invalid": [], "file_conflicts": []}
    out = dict(doc)
    out["format_version"] = FORMAT_V2
    pkgs = []
    seen_files = {}
    for pkg in doc.get("packages") or []:
        p = dict(pkg)
        report["packages"] += 1
        schemas = []
        for s in pkg.get("schemas") or []:
            s2 = dict(s)
            report["schemas"] += 1
            if not s2.get("uid"):
                try:
                    s2["uid"] = schema_uid_for(pkg, s2["id"])
                    report["added_uid"] += 1
                except uidlib.UidError as exc:
                    # 壞紀錄不丟掉：使用者的檔案還在磁碟上，把它從帳本裡抹掉
                    # 等於製造一堆孤兒。留著、標記、讓 UI 有機會說話。
                    s2["uid"] = None
                    report["invalid"].append({"package": pkg.get("id"),
                                              "schema": s2.get("id"),
                                              "error": str(exc)})
            schemas.append(s2)
        p["schemas"] = schemas
        for f in pkg.get("files") or []:
            seen_files.setdefault(f, []).append(pkg.get("id"))
        pkgs.append(p)
    out["packages"] = pkgs
    report["file_conflicts"] = [
        {"file": f, "packages": sorted(set(owners))}
        for f, owners in sorted(seen_files.items()) if len(set(owners)) > 1]
    return out, report


def main(argv=None):
    if not argv:
        argv = sys.argv[1:]
    if not argv:
        print(f"用法: registry.py <{product.STORE_REGISTRY_FILE}> [輸出]",
              file=sys.stderr)
        return 2
    src = argv[0]
    doc = json.load(open(src, encoding="utf-8"))
    new, report = migrate(doc)
    dst = argv[1] if len(argv) > 1 else None
    if dst:
        with open(dst, "w", encoding="utf-8") as f:
            json.dump(new, f, ensure_ascii=False, indent=1)
            f.write("\n")
    else:
        print(json.dumps(new, ensure_ascii=False, indent=1))
    print("遷移 v%s → v%d：%d 個套件、%d 個方案、補上 %d 個 uid"
          % (report["from"], report["to"], report["packages"],
             report["schemas"], report["added_uid"]), file=sys.stderr)
    for c in report["file_conflicts"]:
        print("  ⚠ 檔案被多個套件宣告擁有：%s ← %s"
              % (c["file"], " / ".join(c["packages"])), file=sys.stderr)
    for i in report["invalid"]:
        print("  ⚠ 無法產生 uid：%s / %s（%s）"
              % (i["package"], i["schema"], i["error"]), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
