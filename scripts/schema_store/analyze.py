#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""方案市集 — 從 clone 下來的上游庫解析出「套件」定義。

輸出 <work>/packages.json：每個套件的檔案集合、提供的方案、套件層級 requires、
授權（逐一讀授權檔判定）、分類、建議鍵盤佈局。

相依**不靠猜**，全部從 yaml 實際解析。涵蓋的引用形式見 scan_refs() 的註解。
"""
import io, json, os, re, sys, subprocess

WORK = sys.argv[1]
WORK = os.path.abspath(WORK)          # dir 會被 pack/verify 沿用，一定要絕對路徑
UP = os.path.join(WORK, "upstream")
OPENCC_BUILTIN = set(sys.argv[2].split(",")) if len(sys.argv) > 2 else set()

import yaml

# ────────────────────────────────────────────────────────── 檔案挑選規則 ────
DATA_EXT = (".yaml", ".txt")
SKIP_NAME_RE = re.compile(
    r"^("
    r"weasel(\.custom)?\.yaml|squirrel(\.custom)?\.yaml|trime\.yaml|"
    r".*\.trime\.yaml|.*\.recipe\.yaml|.*\.test\.yaml|"
    r"default\.custom\.yaml|installation\.yaml|user\.yaml|"
    r"README.*|readme.*"
    r")$", re.I)
# 建置腳本 / CI / 前端設定 / lua 外掛 / opencc 自帶詞典，都不是套件內容
SKIP_DIR = {".git", ".github", ".vscode", "scripts", "script", "tools", "src",
            "tests", "test", "doc", "docs", "recipes", "opencc", "OpenCC",
            "lua", "logic", "img", "images", "bin", "node_modules", "release",
            "build", "dist", "assets", "myself"}
LICENSE_FILE_RE = re.compile(r"^(licen[cs]e|copying|unlicen[cs]e)", re.I)
BOPOMOFO_RE = re.compile(r"[ㄅ-ㄯ]")
# 只認真正的韓文音節/日文假名，且要有一定數量 —— 相容用 Jamo（U+3131..U+318E）
# 有些漢語方言方案拿去當音標符號用，會誤判（rime-Sautungva 就中過）。
HANGUL_RE = re.compile(r"[가-힣]")
KANA_RE = re.compile(r"[ぁ-ゖァ-ヺ]")
SCRIPT_MIN = 3
# 大千式的鍵位集合。注意上游的 alphabet 還帶了聲調鍵 " 6347"，
# 所以比對用「集合包含」而不是字串相等 —— 一開始寫成相等，注音就被判成 qwerty 了。
DACHEN_KEYS = set("1qaz2wsxedcrfv5tgbyhnujm8ik,9ol.0p;/-")


def sh(cmd, cwd):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True).stdout.strip()


# ───────────────────────────────────────────────── yaml 讀取（含超大詞庫） ──
_cache = {}


def read_yaml_head(path):
    """.dict.yaml 是「YAML header + `...` + TSV 內文」，只讀到 `...`，
    免得把上百 MB 的詞條餵給 parser。"""
    if path in _cache:
        return _cache[path]
    buf = []
    try:
        with io.open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.rstrip("\r\n") == "...":
                    break
                buf.append(line)
                if len(buf) > 20000:
                    break
    except OSError:
        _cache[path] = (None, "")
        return _cache[path]
    text = "".join(buf)
    try:
        doc = yaml.safe_load(text)
    except Exception:
        doc = None
    _cache[path] = (doc, text)
    return doc, text


# ─────────────────────────────────────────────────────── 引用解析規則 ──────
#   dictionary: X             → X.dict.yaml      translator / reverse_lookup / *@具名*
#   prism: X                  → X.schema.yaml    借用別的方案編出來的 prism
#   schema/dependencies: [X]  → X.schema.yaml    RIME 官方的反查相依宣告
#   import_tables / packs     → X.dict.yaml
#   vocabulary: X             → X.txt
#   use_preset_vocabulary:true→ essay.txt        （預設語料庫）
#   import_preset: X          → X.yaml           punctuator / key_binder / speller
#   __include: / __patch:     → 前綴的 yaml 檔
#   opencc_config: X.json     → 另外記錄（.ocd2 由 APK 內建，不打包）
def scan_refs(doc, text):
    files, opencc = set(), set()
    lua = bool(re.search(r"\blua_(translator|filter|processor|segmentor)\b", text))

    def add_yaml(name):
        name = str(name).strip().strip("\"'")
        if name:
            files.add(name if name.endswith(".yaml") else name + ".yaml")

    def walk(node):
        if isinstance(node, dict):
            for k, v in node.items():
                ks = str(k)
                if ks == "dictionary" and isinstance(v, str) and v.strip():
                    files.add(v.strip() + ".dict.yaml")
                elif ks == "prism" and isinstance(v, str) and v.strip():
                    files.add(v.strip() + ".schema.yaml")
                elif ks == "dependencies" and isinstance(v, list):
                    for x in v:
                        if isinstance(x, str) and x.strip():
                            files.add(x.strip() + ".schema.yaml")
                elif ks in ("import_tables", "packs") and isinstance(v, list):
                    for x in v:
                        if isinstance(x, str) and x.strip():
                            files.add(x.strip() + ".dict.yaml")
                elif ks == "vocabulary" and isinstance(v, str) and v.strip():
                    files.add(v.strip() + ".txt")
                elif ks == "use_preset_vocabulary" and v is True:
                    files.add("essay.txt")
                elif (ks == "import_preset" or ks.endswith("/import_preset")) \
                        and isinstance(v, str) and v.strip():
                    add_yaml(v)
                elif ks in ("__include", "__patch") and isinstance(v, str):
                    if ":" in v:                      # 有冒號才是「別的檔案」
                        add_yaml(v.split(":", 1)[0])
                elif ks == "opencc_config" and isinstance(v, str) and v.strip():
                    opencc.add(v.strip())
                walk(v)
        elif isinstance(node, list):
            for x in node:
                walk(x)

    walk(doc)

    # 純文字兜底：yaml parse 失敗，或引用出現在 parser 取不到的位置時仍要抓到。
    for m in re.finditer(r"^[ \t]*__(?:include|patch):[ \t]*([^\s#]+)", text, re.M):
        raw = m.group(1).strip().strip("\"'")
        if ":" in raw:
            ref = raw.split(":", 1)[0]
            if ref and not ref.startswith("/"):
                add_yaml(ref)
    for m in re.finditer(r"^\s*dictionary:\s*([^\s#]+)\s*$", text, re.M):
        v = m.group(1).strip().strip("\"'")
        if v and v not in ('""', "''"):
            files.add(v + ".dict.yaml")
    for m in re.finditer(r"^\s*opencc_config:\s*([^\s#]+)", text, re.M):
        opencc.add(m.group(1).strip().strip("\"'"))
    return files, opencc, lua


# ──────────────────────────────────────────────────────────── 授權判定 ────
LICENSE_SIGS = [
    ("GNU LESSER GENERAL PUBLIC LICENSE", "Version 3", "LGPL-3.0"),
    ("GNU LESSER GENERAL PUBLIC LICENSE", "Version 2.1", "LGPL-2.1"),
    ("GNU AFFERO GENERAL PUBLIC LICENSE", "Version 3", "AGPL-3.0"),
    ("GNU GENERAL PUBLIC LICENSE", "Version 3", "GPL-3.0"),
    ("GNU GENERAL PUBLIC LICENSE", "Version 2", "GPL-2.0"),
    ("Apache License", "Version 2.0", "Apache-2.0"),
    ("Attribution-ShareAlike 4.0 International", "", "CC-BY-SA-4.0"),
    ("Attribution 4.0 International", "", "CC-BY-4.0"),
    ("CC0 1.0 Universal", "", "CC0-1.0"),
    ("ODC Open Database License", "", "ODbL-1.0"),
    ("Open Database License", "", "ODbL-1.0"),
    ("MIT License", "", "MIT"),
    ("Permission is hereby granted, free of charge", "", "MIT"),
    ("Redistribution and use in source and binary forms", "Neither the name", "BSD-3-Clause"),
    ("Redistribution and use in source and binary forms", "", "BSD-2-Clause"),
]


def detect_license(repo_dir):
    out = []
    for name in sorted(os.listdir(repo_dir)):
        if not LICENSE_FILE_RE.match(name):
            continue
        p = os.path.join(repo_dir, name)
        if not os.path.isfile(p):
            continue
        head = io.open(p, encoding="utf-8", errors="replace").read(8000)
        spdx = "UNKNOWN"
        for a, b, sid in LICENSE_SIGS:
            if a in head and (not b or b in head):
                spdx = sid
                break
        out.append({"file": name, "spdx": spdx,
                    "first_line": next((l.strip() for l in head.splitlines() if l.strip()), "")})
    return out


def detect_or_later(repo_dir):
    for root, dirs, files in os.walk(repo_dir):
        dirs[:] = [d for d in dirs if d != ".git"]
        for fn in files:
            if not fn.endswith((".yaml", ".md", ".txt", ".markdown")):
                continue
            p = os.path.join(root, fn)
            try:
                if os.path.getsize(p) > 4_000_000:
                    continue
                if "any later version" in io.open(p, encoding="utf-8",
                                                  errors="replace").read(20000):
                    return True
            except OSError:
                continue
    return False


# ────────────────────────────────────────────────────────────── 分類判斷 ──
# 依據（寫進報告）：
#   1. 以 rppi（RIME 官方套件索引）的分類路徑為第一來源 —— 那是上游自己的判斷。
#   2. rppi 沒收錄的，依該庫提供的方案性質手動歸類，理由記在 CATEGORY_REASON。
#   3. 不提供任何方案（schemas 為空）者一律 essential（hidden），因為它只會被
#      requires 指名，不該出現在切換清單。
RPPI_CATEGORY = {
    "Chinese/Mandarin": "mandarin",
    "Chinese/Shape":    "mandarin",   # 字形類（倉頡/五筆/速成/行列/筆畫）仍是打華語
    "Chinese/Cantonese": "topolect",
    "Chinese/Wu":       "topolect",
    "Chinese/Hakka":    "topolect",
    "Chinese/Hokkien":  "topolect",
    "Chinese/Xiang":    "topolect",
    "Chinese/Old":      "topolect",   # 中古/上古音是漢語的歷史音系，不是其他語系
    "Japanese":         "other",
    "Korean":           "other",
    "English":          "other",
    "Other":            "other",
    "Collection":       "mandarin",
}
MANUAL_CATEGORY = {
    "jyutping": ("topolect", "粵語拼音方案，rppi 未收錄（被較新的 rime-cantonese 取代）"),
    "prelude":  ("essential", "只提供 default/標點/按鍵綁定，無方案"),
    "essay":    ("essential", "只提供語料庫 essay.txt，無方案"),
    "essay-simp": ("essential", "只提供簡體語料庫，無方案"),
    "custom":   ("essential", "只是 plum 的 recipe 範例，無方案"),
    "emoji":    ("essential", "opencc filter 外掛，無方案"),
    "emoji-cantonese": ("essential", "opencc filter 外掛，無方案"),
}


def pkg_id(slug):
    name = slug.split("/")[-1]
    name = re.sub(r"^[Rr]ime[-_]", "", name)
    name = re.sub(r"[-_.\s]+", "-", name).strip("-").lower()
    return name or slug.split("/")[-1].lower()


# ─────────────────────────────────────────────────────────────── 主流程 ────
repos = json.load(open(os.path.join(WORK, "repos.json")))
pkgs, excluded = [], []

for r in repos:
    slug = r["repo"]
    d = os.path.join(UP, slug.replace("/", "__"))
    pid = pkg_id(slug)
    if not os.path.isdir(d):
        excluded.append({"id": pid, "repo": slug, "stage": "fetch",
                         "reason": "clone 失敗"})
        continue

    # ── 授權：逐一讀授權檔。沒有授權檔就不散布（規範 §2 要求 zip 內含 LICENSE）。
    lic = detect_license(d)
    if not lic:
        excluded.append({"id": pid, "repo": slug, "stage": "license",
                         "reason": "上游庫內找不到任何授權檔（LICENSE/COPYING），"
                                   "無法確認散布條件，也無法滿足規範 §2「zip 必須含 LICENSE」"})
        continue

    # ── 候選檔案（basename -> 最淺的相對路徑）
    cand = {}
    for root, dirs, files in os.walk(d):
        dirs[:] = [x for x in dirs if x not in SKIP_DIR]
        rel_root = os.path.relpath(root, d)
        depth = 0 if rel_root == "." else rel_root.count(os.sep) + 1
        for fn in files:
            if not fn.endswith(DATA_EXT) or SKIP_NAME_RE.match(fn) \
               or LICENSE_FILE_RE.match(fn):
                continue
            p = os.path.join(root, fn)
            if os.path.islink(p):
                continue
            rel = fn if rel_root == "." else os.path.join(rel_root, fn)
            prev = cand.get(fn)
            if prev is None or depth < prev[0]:
                cand[fn] = (depth, rel)

    # ── 這個庫「提供」哪些方案：優先取 root 層的 *.schema.yaml
    schema_files = {fn: v for fn, v in cand.items() if fn.endswith(".schema.yaml")}
    root_schemas = {fn: v for fn, v in schema_files.items() if v[0] == 0}
    advertised = root_schemas or schema_files

    # ── 閉包：從 root 層所有資料檔出發，沿引用把需要的檔案拉進來
    closure, queue = set(), []
    for fn, (depth, rel) in cand.items():
        if depth == 0:
            closure.add(fn)
            queue.append(fn)
    for fn in advertised:
        if fn not in closure:
            closure.add(fn)
            queue.append(fn)

    external, opencc_refs, needs_lua = set(), set(), False
    while queue:
        fn = queue.pop()
        doc, text = read_yaml_head(os.path.join(d, cand[fn][1]))
        refs, occ, lua = scan_refs(doc, text)
        needs_lua = needs_lua or lua
        opencc_refs |= occ
        for ref in refs:
            key = ref if ref in cand else os.path.basename(ref)
            if key in cand:
                if key not in closure:
                    closure.add(key)
                    queue.append(key)
            else:
                external.add(ref)

    # ── 方案 metadata + speller 鍵位
    schemas, alphabets, bopomofo_any, nonascii_alpha = [], {}, False, {}
    for fn in sorted(advertised):
        doc, text = read_yaml_head(os.path.join(d, cand[fn][1]))
        if not isinstance(doc, dict):
            continue
        sch = doc.get("schema") or {}
        sid = sch.get("schema_id") or fn[:-len(".schema.yaml")]
        sname = sch.get("name") or sid
        alpha = ((doc.get("speller") or {}).get("alphabet")
                 if isinstance(doc.get("speller"), dict) else None)
        if alpha is not None:
            alpha = str(alpha)
            alphabets[sid] = alpha
            if any(ord(c) > 127 for c in alpha):
                nonascii_alpha[sid] = alpha
        if BOPOMOFO_RE.search(text):
            bopomofo_any = True
        # 大千式的判準：schema 直接宣告大千鍵位，或 algebra 套了 zhuyin 的
        # keymap_bopomofo（bopomofo_tw / bopomofo_express 走的是後者）。
        dachen = ("keymap_bopomofo" in text
                  or (alpha and DACHEN_KEYS <= set(alpha)))
        schemas.append({"id": str(sid), "name": str(sname), "file": fn,
                        "alphabet": alpha,
                        "layout": "bopomofo-dachen" if dachen else "qwerty",
                        "script": ("hangul" if len(HANGUL_RE.findall(text)) >= SCRIPT_MIN
                                   else "kana" if len(KANA_RE.findall(text)) >= SCRIPT_MIN
                                   else None)})

    pkgs.append({
        "id": pid, "repo": slug, "upstream": f"https://github.com/{slug}",
        "dir": d, "commit": sh(["git", "rev-parse", "HEAD"], d),
        "license_files": lic, "or_later": detect_or_later(d),
        "github_license": r.get("github_license"),
        "rppi_license": r.get("rppi_license"),
        "rppi_path": r.get("rppi_path"), "rppi_name": r.get("rppi_name"),
        "rppi_labels": r.get("rppi_labels") or [],
        "files": sorted(closure),
        "paths": {fn: cand[fn][1] for fn in sorted(closure)},
        "schemas": schemas,
        "external_refs": sorted(external),
        "opencc_refs": sorted(opencc_refs),
        "needs_lua": needs_lua,
        "alphabets": alphabets,
        "nonascii_alphabets": nonascii_alpha,
        "bopomofo": bopomofo_any,
    })

# ── id 衝突
seen = {}
for p in pkgs:
    seen.setdefault(p["id"], []).append(p["repo"])
for pid, rs in seen.items():
    if len(rs) > 1:
        print(f"[錯誤] 套件 id 衝突: {pid} <- {rs}", file=sys.stderr)
        sys.exit(1)

by_id = {p["id"]: p for p in pkgs}

# ── 檔案 -> 提供者索引（同名檔可能多家提供，rime 官方庫優先）
providers = {}
for p in pkgs:
    for fn in p["files"]:
        providers.setdefault(fn, []).append(p["id"])


def pick(fn, want):
    cands = providers.get(fn, [])
    if not cands:
        return None
    official = [c for c in cands if by_id[c]["repo"].startswith("rime/")]
    pool = official or cands
    base = fn.split(".")[0]
    exact = [c for c in pool if c == base.replace("_", "-")]
    return (exact or sorted(pool))[0]


# ── 檔案層級相依 → 套件層級 requires
for p in pkgs:
    req, missing = set(), []
    for fn in p["external_refs"]:
        owner = pick(fn, p["id"])
        if owner and owner != p["id"]:
            req.add(owner)
        elif not owner:
            missing.append(fn)
    # librime 的 Deployer 一定要讀 default.yaml（schema_list/menu/切換鍵都在裡面），
    # 而它只有 rime-prelude 提供。這條是隱含相依，不會出現在任何 __include 裡。
    if p["id"] != "prelude":
        req.add("prelude")
    p["requires"] = sorted(req)
    p["missing_refs"] = sorted(set(missing))

# ── 循環相依偵測（Tarjan 求 SCC）
index, low, onstk, stk, sccs = {}, {}, set(), [], []
counter = [0]


def strong(v):
    index[v] = low[v] = counter[0]
    counter[0] += 1
    stk.append(v)
    onstk.add(v)
    for w in by_id[v]["requires"]:
        if w not in by_id:
            continue
        if w not in index:
            strong(w)
            low[v] = min(low[v], low[w])
        elif w in onstk:
            low[v] = min(low[v], index[w])
    if low[v] == index[v]:
        comp = []
        while True:
            w = stk.pop()
            onstk.discard(w)
            comp.append(w)
            if w == v:
                break
        if len(comp) > 1:
            sccs.append(sorted(comp))


sys.setrecursionlimit(10000)
for p in pkgs:
    if p["id"] not in index:
        strong(p["id"])

# ── 遞迴展開（含循環保護）
def closure_of(pid):
    seen_, stack = set(), [pid]
    while stack:
        x = stack.pop()
        if x in seen_ or x not in by_id:
            continue
        seen_.add(x)
        stack += by_id[x]["requires"]
    return sorted(seen_)


for p in pkgs:
    p["requires_closure"] = closure_of(p["id"])

# ── 分類
for p in pkgs:
    if not p["schemas"]:
        p["category"] = "essential"
        p["category_reason"] = MANUAL_CATEGORY.get(
            p["id"], ("essential", "不提供任何方案，只作為相依"))[1]
    elif p["id"] in MANUAL_CATEGORY:
        p["category"], p["category_reason"] = MANUAL_CATEGORY[p["id"]]
    elif p["rppi_path"] in RPPI_CATEGORY:
        p["category"] = RPPI_CATEGORY[p["rppi_path"]]
        p["category_reason"] = f"rppi 官方索引分類 {p['rppi_path']}"
    else:
        p["category"] = "other"
        p["category_reason"] = "rppi 未收錄且非漢語方案，暫歸其他語系"

# ── 建議鍵盤佈局
# 判準是方案的 speller/alphabet 與 algebra：
#   純 ASCII 字母（倉頡、五筆、粵拼、各式拼音）→ qwerty
#   大千鍵位（注音）→ bopomofo-dachen。這條不記的話，使用者導入注音後
#   會看到一個打不出注音的 QWERTY 鍵盤，比沒收錄還糟。
for p in pkgs:
    layouts = [s["layout"] for s in p["schemas"]]
    nonascii = p["nonascii_alphabets"]
    notes = []
    if "bopomofo-dachen" in layouts:
        layout = "bopomofo-dachen"
        if any(l != "bopomofo-dachen" for l in layouts):
            other = [s["id"] for s in p["schemas"] if s["layout"] != "bopomofo-dachen"]
            notes.append("同一套件裡的 %s 用的是拉丁字母鍵位，那幾個方案 QWERTY 就夠。"
                         % "、".join(other))
    else:
        layout = "qwerty"
    if nonascii:
        notes.append("speller/alphabet 含非 ASCII 字元（%s），QWERTY 上沒有這些鍵，"
                     "需要專屬佈局。" % "、".join(sorted(nonascii)))
    scripts = {s["script"] for s in p["schemas"] if s["script"]}
    if layout == "qwerty" and scripts:
        zh = {"hangul": "韓文字母", "kana": "日文假名"}
        notes.append("輸入碼是拉丁字母，QWERTY 打得出來；但輸出是%s，"
                     "鍵帽若能顯示對應字母會更好用，屬於待辦的新佈局。"
                     % "、".join(zh[x] for x in sorted(scripts)))
    elif layout == "qwerty" and not notes and p["schemas"]:
        notes.append("輸入碼為英文字母，QWERTY 佈局可用")
    p["recommended_layout"] = layout
    p["layout_note"] = " ".join(notes) if notes else None

# ── 需要 librime-lua 的套件：本專案的 librime 未編入 lua 外掛。
# 這類方案部署會「成功」但引擎少了 translator/filter，按下去沒有候選 ——
# deploy-only 閘門抓不到，所以在這裡就擋掉，不讓它有機會混進索引。
keep = []
for p in pkgs:
    if p["needs_lua"]:
        excluded.append({"id": p["id"], "repo": p["repo"], "stage": "analyze",
                         "reason": "方案使用 lua_translator/lua_filter 等元件，"
                                   "需要 librime-lua 外掛；本專案的 librime 未編入，"
                                   "部署會假成功但打不出字"})
    else:
        keep.append(p)
pkgs = keep
by_id = {p["id"]: p for p in pkgs}

# ── 跨套件衝突：解壓後都落在同一個 user_data_dir，同名檔會互相覆蓋。
file_owners, schema_owners = {}, {}
for p in pkgs:
    for fn in p["files"]:
        file_owners.setdefault(fn, []).append(p["id"])
    for sc in p["schemas"]:
        schema_owners.setdefault(sc["id"], []).append(p["id"])
conflicts = {
    "files": {k: v for k, v in file_owners.items() if len(v) > 1},
    "schema_ids": {k: v for k, v in schema_owners.items() if len(v) > 1},
}

json.dump({"packages": pkgs, "excluded": excluded, "cycles": sccs,
           "conflicts": conflicts},
          open(os.path.join(WORK, "packages.json"), "w"),
          ensure_ascii=False, indent=1)

print(f"可打包候選 {len(pkgs)} 個；因授權/抓取排除 {len(excluded)} 個")
if sccs:
    print("循環相依 SCC:", sccs)
for p in sorted(pkgs, key=lambda x: x["id"]):
    print(f"{p['id']:22s} {p['category']:9s} schemas={len(p['schemas']):2d} "
          f"files={len(p['files']):3d} req={','.join(p['requires']) or '-':30s} "
          f"lic={'+'.join(l['spdx'] for l in p['license_files'])}"
          f"{' LUA' if p['needs_lua'] else ''}"
          f"{' OPENCC:' + ','.join(sorted(set(p['opencc_refs']) - OPENCC_BUILTIN)) if set(p['opencc_refs']) - OPENCC_BUILTIN else ''}"
          f"{' MISSING:' + ','.join(p['missing_refs'][:4]) if p['missing_refs'] else ''}")
