#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_fake_store.py —— 產生「假的方案市集」與惡意 zip 測試資料。

為什麼需要這支東西
------------------
伺服器側那條線正在產生真的 index.json 與套件 zip，但行動端不能空等。
這支腳本產生一份**完全符合 docs/schema-store.md §1 的假索引**與對應的
套件，讓整條導入流程（相依展開 → 下載 → sha256 → zip 安全檢查 → 解壓 →
schema_list → 部署 → 回滾）可以在真索引上線前就做完做對。
真索引一上線，把市集畫面的「索引來源」換掉即可，程式不必動。

產物
----
  <out>/index.json                 假索引
  <out>/packages/*.zip             假套件
  <out>/malicious/*.zip            惡意 zip（單元測試與人工驗證共用）

假套件刻意設計成能驗證四件事
----------------------------
  rq-demo-base   只提供詞典、schemas 為空 → 驗證「只作為相依的元件套件」
  rq-demo        requires rq-demo-base    → 驗證遞迴相依展開與真的能打字
  rq-nodict      詞典根本不存在           → 驗證**部署前**就能指出缺哪一本詞典
  rq-brokendict  詞典存在但 YAML 壞掉     → 驗證**部署失敗後的回滾**
                 （這是預檢不可能事先知道的情形，也就是回滾唯一存在的理由）

用法
----
  python3 make_fake_store.py --out android/testdata/store \\
      --base-url http://127.0.0.1:8099/packages/
"""

import argparse
import hashlib
import json
import os
import shutil
import stat
import time
import zipfile

# ─────────────────────────────────────────────────────────── 套件內容 ───

LICENSE = """GNU General Public License v3.0 (SPDX: GPL-3.0-or-later)

這是假索引的測試套件，內容為本專案自製，非上游 RIME 方案。
"""


def upstream(name, commit):
    return (
        "upstream: https://example.invalid/rimequad/%s\n"
        "commit:   %s\n"
        "packaged: %s\n"
        "note:     由 android/testdata/make_fake_store.py 產生的測試套件，\n"
        "          不是真的上游方案。\n" % (name, commit, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    )


# 最小可用的 table_translator 方案。刻意不用 essay / prelude 之外的東西，
# 這樣它只依賴自己那本詞典，相依關係才乾淨。
DEMO_SCHEMA = """# Rime schema
# encoding: utf-8

schema:
  schema_id: rq_demo
  name: 市集示範
  version: "1.0"
  author:
    - RimeQuad
  description: |
    假索引用的最小方案，用來驗證方案市集的導入流程。

switches:
  - name: ascii_mode
    reset: 0
    states: [ 中, A ]

engine:
  processors:
    - ascii_composer
    - recognizer
    - key_binder
    - speller
    - selector
    - navigator
    - express_editor
  segmentors:
    - ascii_segmentor
    - matcher
    - abc_segmentor
    - fallback_segmentor
  translators:
    - echo_translator
    - table_translator

translator:
  dictionary: rq_demo
  enable_sentence: false

speller:
  alphabet: zyxwvutsrqponmlkjihgfedcba

menu:
  page_size: 5
"""

DEMO_DICT = """# Rime dictionary
# encoding: utf-8
---
name: rq_demo
version: "1.0"
sort: by_weight
...

你好\tnihao\t100
市集\tshiji\t100
方案\tfangan\t100
測試\tceshi\t100
輸入法\tshurufa\t100
方案市集\tzzz\t100
"""

# 註：`zzz` → 方案市集 這一條是刻意放的「指紋」。驗證時要證明的是
# 「**新裝的 rq_demo 方案**在工作」，而不只是「輸入法在工作」——
# 若用 nihao→你好 當斷言，luna_pinyin 也打得出來，證明不了任何事。
# zzz 在拼音方案裡打不出東西，只有 rq_demo 認得。

# 指向一本**不存在**的詞典。預檢（SchemaPreflight）應該在改 schema_list
# 之前就抓到，並指名 rq_absent.dict.yaml。
NODICT_SCHEMA = DEMO_SCHEMA.replace("rq_demo", "rq_nodict").replace(
    "dictionary: rq_nodict", "dictionary: rq_absent"
)

# 詞典檔**存在**（所以預檢會過），但 YAML 頭壞掉，librime 編譯時才會失敗。
# 這是回滾路徑唯一能被誠實觸發的方式：預檢不可能預先知道 librime 編不編得起來。
BAD_SCHEMA = DEMO_SCHEMA.replace("rq_demo", "rq_bad").replace("市集示範", "壞掉的示範")
BAD_DICT = """# Rime dictionary
# encoding: utf-8
---
name: rq_bad
version: "1.0"
sort: by_weight
columns: [ text, code, weight
  這一行讓 YAML 解析失敗: [ 未關閉的流式序列
...

你好\tnihao\t100
"""

PACKAGES = [
    {
        "id": "rq-demo-base",
        "name": "示範詞庫（基礎元件）",
        "category": "essential",
        "description": "rq_demo 方案的詞典。只作為相依，不會出現在方案切換清單裡。",
        "commit": "b0a5e01",
        "files": {"rq_demo.dict.yaml": DEMO_DICT},
        "schemas": [],
        "requires": [],
        "deployed": True,
        "probe": None,
        "recommended_layout": None,
        "layout_note": None,
        "recommended": False,
    },
    {
        "id": "rq-demo",
        "name": "市集示範方案",
        "category": "mandarin",
        "description": "驗證用的最小拼音方案。輸入 nihao 應該得到「你好」。",
        "commit": "d3m0a11",
        "files": {"rq_demo.schema.yaml": DEMO_SCHEMA},
        "schemas": [{"id": "rq_demo", "name": "市集示範"}],
        "requires": ["rq-demo-base"],
        "deployed": True,
        "probe": {"schema": "rq_demo", "keys": "nihao", "expect": "你好"},
        "recommended_layout": "qwerty",
        "layout_note": None,
        "recommended": True,
    },
    {
        "id": "rq-nodict",
        "name": "缺詞典的示範方案",
        "category": "mandarin",
        "description": "刻意缺少詞典的套件，用來驗證錯誤訊息會指名缺的是哪一本。",
        "commit": "0d1c700",
        "files": {"rq_nodict.schema.yaml": NODICT_SCHEMA},
        "schemas": [{"id": "rq_nodict", "name": "缺詞典示範"}],
        "requires": [],
        "deployed": False,
        "probe": None,
        "recommended_layout": "qwerty",
        "layout_note": "這個套件本來就是壞的，用來驗證失敗路徑。",
        "recommended": False,
    },
    {
        "id": "rq-brokendict",
        "name": "詞典壞掉的示範方案",
        "category": "mandarin",
        "description": "詞典檔在，但 YAML 壞掉；預檢會過、librime 部署會失敗，用來驗證回滾。",
        "commit": "bad0d1c",
        "files": {"rq_bad.schema.yaml": BAD_SCHEMA, "rq_bad.dict.yaml": BAD_DICT},
        "schemas": [{"id": "rq_bad", "name": "壞掉的示範"}],
        "requires": [],
        "deployed": False,
        "probe": None,
        "recommended_layout": "qwerty",
        "layout_note": "這個套件本來就是壞的，用來驗證部署失敗會回滾 schema_list。",
        "recommended": False,
    },
]

CATEGORIES = [
    {"id": "mandarin", "name": "華語", "order": 1},
    {"id": "topolect", "name": "方言", "order": 2},
    {"id": "other", "name": "其他語系", "order": 3},
    {"id": "essential", "name": "基礎元件", "order": 9, "hidden": True},
]


def write_zip(path, files, mtime=(1980, 1, 1, 0, 0, 0)):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(files):
            info = zipfile.ZipInfo(name, date_time=mtime)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o644 << 16)
            info.create_system = 3  # Unix
            z.writestr(info, files[name])


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


# ────────────────────────────────────────────────────────── 惡意 zip ───

def make_malicious(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    made = []

    # ① 路徑穿越。解壓工具若照單全收，這個檔案會落在 app 沙盒之外。
    p = os.path.join(out_dir, "evil-traversal.zip")
    write_zip(p, {
        "rq_evil.schema.yaml": "# 看起來人畜無害\n",
        "../../evil.yaml": "# 這一份不該被寫出來\n",
    })
    made.append(p)

    # ② 絕對路徑，同樣是穿越的一種。
    p = os.path.join(out_dir, "evil-absolute.zip")
    write_zip(p, {"/tmp/evil-absolute.yaml": "# 絕對路徑\n"})
    made.append(p)

    # ③ 符號連結。zip 用 external attributes 的 unix mode 標記，
    #    entry 的內容就是連結目標字串。
    p = os.path.join(out_dir, "evil-symlink.zip")
    with zipfile.ZipFile(p, "w", zipfile.ZIP_DEFLATED) as z:
        info = zipfile.ZipInfo("passwd.yaml", date_time=(1980, 1, 1, 0, 0, 0))
        info.create_system = 3
        info.external_attr = (stat.S_IFLNK | 0o777) << 16
        z.writestr(info, "/etc/passwd")
        ok = zipfile.ZipInfo("rq_evil.schema.yaml", date_time=(1980, 1, 1, 0, 0, 0))
        ok.create_system = 3
        ok.external_attr = 0o644 << 16
        z.writestr(ok, "# 正常檔案\n")
    made.append(p)

    # ④ 解壓炸彈：8MB 的零壓成幾 KB，壓縮比遠超過上限。
    p = os.path.join(out_dir, "evil-bomb.zip")
    write_zip(p, {"bomb.txt": "\0" * (8 * 1024 * 1024)})
    made.append(p)

    # ⑤ 可執行檔（副檔名白名單）。
    p = os.path.join(out_dir, "evil-executable.zip")
    write_zip(p, {
        "rq_evil.schema.yaml": "# 陪襯\n",
        "libpwn.so": "\x7fELF fake payload",
    })
    made.append(p)

    # ⑥ **中央目錄說謊**：宣告解壓後只有 10 bytes，實際是 4MB。
    #    這一個專門用來證明「只信宣告值的檢查等於沒檢查」——
    #    擋下它的是解壓迴圈裡的硬性計數，不是事前檢查。
    p = os.path.join(out_dir, "evil-lying-size.zip")
    write_zip(p, {"liar.txt": "A" * (4 * 1024 * 1024)})
    patch_declared_sizes(p, b"liar.txt", 10)
    made.append(p)

    return made


def patch_declared_sizes(path, entry_name, fake_uncompressed):
    """把中央目錄裡某個 entry 的 uncompressed size 改小。"""
    with open(path, "rb") as f:
        data = bytearray(f.read())
    sig = b"PK\x01\x02"
    i = 0
    patched = 0
    while True:
        i = data.find(sig, i)
        if i < 0:
            break
        name_len = int.from_bytes(data[i + 28:i + 30], "little")
        name = bytes(data[i + 46:i + 46 + name_len])
        if name == entry_name:
            data[i + 24:i + 28] = fake_uncompressed.to_bytes(4, "little")
            patched += 1
        i += 4
    if patched == 0:
        raise SystemExit("找不到要修改的 entry: %r" % entry_name)
    with open(path, "wb") as f:
        f.write(data)


# ────────────────────────────────────────────────────────────── main ───

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--base-url", default="http://127.0.0.1:8099/packages/")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    pkg_dir = os.path.join(out, "packages")
    if os.path.isdir(pkg_dir):
        shutil.rmtree(pkg_dir)
    os.makedirs(pkg_dir)

    entries = []
    for spec in PACKAGES:
        files = dict(spec["files"])
        files["LICENSE"] = LICENSE
        files["UPSTREAM.txt"] = upstream(spec["id"], spec["commit"])
        fname = "%s-%s.zip" % (spec["id"], spec["commit"])
        path = os.path.join(pkg_dir, fname)
        write_zip(path, files)

        entry = {
            "id": spec["id"],
            "name": spec["name"],
            "category": spec["category"],
            "description": spec["description"],
            "upstream": "https://example.invalid/rimequad/%s" % spec["id"],
            "upstream_commit": spec["commit"],
            "license": "GPL-3.0-or-later",
            "file": fname,
            "size": os.path.getsize(path),
            "sha256": sha256_of(path),
            "schemas": spec["schemas"],
            "requires": spec["requires"],
            "verified": {"deployed": spec["deployed"]},
            "recommended": spec["recommended"],
        }
        if spec["probe"]:
            entry["verified"]["probe"] = spec["probe"]
        if spec["recommended_layout"]:
            entry["recommended_layout"] = spec["recommended_layout"]
        if spec["layout_note"]:
            entry["layout_note"] = spec["layout_note"]
        entries.append(entry)

    index = {
        "format_version": 1,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "base_url": args.base_url,
        "categories": CATEGORIES,
        "packages": entries,
    }
    with open(os.path.join(out, "index.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False, indent=2)
        f.write("\n")

    made = make_malicious(os.path.join(out, "malicious"))

    print("索引:", os.path.join(out, "index.json"))
    for e in entries:
        print("  套件 %-16s %8d bytes  %s" % (e["id"], e["size"], e["sha256"][:16]))
    for m in made:
        print("  惡意 %-24s %d bytes" % (os.path.basename(m), os.path.getsize(m)))


if __name__ == "__main__":
    main()
