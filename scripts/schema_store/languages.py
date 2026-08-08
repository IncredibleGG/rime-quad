#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
languages.py — 給每一個 librime 方案標上 BCP 47 語言標記。

為什麼要有這支
─────────────────────────────────────────────────────────────────────────────
鍵盤類型選單要依語言分組，但**資料裡沒有語言資訊**：

  · `rs_schema_list()` 只給 `(id, name)`；
  · librime 的 `schema.yaml` 沒有語言／地區欄位；
  · 方案市集索引的 `category` 是「拼音類／形碼類」而不是語言，
    而且不涵蓋隨 APK 出貨的內建方案。

於是 KeyboardTypes.kt 只能從 id 後綴與方案名的字面推。能動，但脆：
`_tw` 後綴是慣例不是規範，方案名是自由字串，而「名字裡有漢字」把日文的
「河童五筆」也算成中文。

本檔把那件事變成**資料**，並且讓每一筆判定都說得出依據。

判定順序（規範性；每一層都把依據寫進 source/rule/why）
─────────────────────────────────────────────────────────────────────────────
  0. data/languages.yaml 的 `schemas:` —— 人工判定，附理由。只用在自動規則
       會判錯的地方：工具方案（不產出文字）、套件裡的異語言子方案、
       地區只有人看得出來的。鍵可以是 uid 或裸 id（見該檔說明）。
  1. 語言 subtag：rppi 的分類路徑（`Chinese/Cantonese` → `yue`…）。
       rppi 沒收錄的套件走 `packages:` 的 `lang`（人工，附理由）。
  2. script / region：
       a. 方案名裡的地區字樣（臺灣正體 / 简化字 / 香港 …）；
       b. **字集探針**：解析該方案自己的 `translator/dictionary`，
          把對應的 `*.dict.yaml` 拿來數「繁體專用字」與「簡體專用字」。
       c. 自己沒有詞庫時（注音、雙拼、宮保這類借別人詞庫的方案），
          改探針該套件 `requires` 的套件 —— 借誰的詞庫就跟誰同字集。
  3. 以上都判不出來 → `und`。**不猜。**

`und` 的兩種來源在報告裡是分開的，因為處置方式不同：
  · 「不是語言」（IPA 音標、工具方案）—— 資料裡明寫 `not_a_language: true`，
    標 `und` 是正確答案，不必修；
  · 「查不到」—— 是缺口，要補資料。涵蓋率報告算的是這一種。

⚠ 一切判定的鍵是 **uid**（`<套件>/<方案>`），不是裸的 schema id
─────────────────────────────────────────────────────────────────────────────
`double_pinyin` 同時存在於 `double-pinyin`（繁體詞庫）與 `ice`（簡體詞庫），
字集相反。用裸 id 當鍵的表沒有辦法同時表達兩者。見 uid.py 的檔頭。

用法
    python3 languages.py <WORK>            # 印出報告（涵蓋率、und 清單）
    python3 languages.py <WORK> --asset <out.json>   # 產生隨 APK 出貨的對照表
    python3 languages.py <WORK> --json     # 全部判定（含依據）以 JSON 印出
    python3 languages.py <WORK> --max-unknown N   # 未知超過 N 個就 exit 1
"""

from __future__ import annotations

import argparse
import datetime
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uid as uidlib   # noqa: E402

FORMAT_VERSION = 1
# 加欄位不加版本：舊的讀取端（Android 的 SchemaLanguages.parse 要求
# format_version == 1）必須照樣讀得動。相容性規則見 docs/schema-store.md §1.1。
FORMAT_MINOR = 1

DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
DATA_FILE = os.path.join(DATA_DIR, "languages.yaml")

# 來源可信度由高到低。多個成分時取**最低**的那一個。
SOURCE_RANK = {"upstream": 0, "curated": 1, "derived": 2, "unknown": 3}


def _worst(kinds):
    return max(kinds, key=lambda k: SOURCE_RANK.get(k, 9)) if kinds else "unknown"


# ── 資料檔 ──────────────────────────────────────────────────────────────────

class LangData(object):
    """data/languages.yaml 的記憶體形式。**程式裡不得再有第二份判定資料。**"""

    def __init__(self, doc):
        self.languages = doc["languages"]
        self.known_tags = {l["tag"] for l in self.languages}
        self.rppi_paths = doc.get("rppi_paths") or {}
        self.packages = doc.get("packages") or {}
        self.schemas = doc.get("schemas") or {}
        self.builtin = doc.get("builtin") or []
        self.name_marks = doc.get("name_marks") or []
        pr = doc.get("probe") or {}
        self.min_sample = int(pr.get("min_sample", 50))
        self.ratio = float(pr.get("ratio", 2.0))
        self.read_bytes = int(pr.get("read_bytes", 600000))
        trad, simp = pr.get("trad") or "", pr.get("simp") or ""
        self.trad_only = set(trad) - set(simp)
        self.simp_only = set(simp) - set(trad)
        self._trad_raw, self._simp_raw = trad, simp

    def validate(self):
        """資料檔自己的健全性。壞資料要在這裡就死，不要一路長到索引裡。"""
        errs = []
        if len(self._trad_raw) != len(self._simp_raw):
            errs.append("probe.trad 與 probe.simp 長度不同（%d vs %d）"
                        % (len(self._trad_raw), len(self._simp_raw)))
        if len(self.trad_only) < 50 or len(self.simp_only) < 50:
            errs.append("字集探針的對照字太少（繁 %d / 簡 %d），判別會不穩"
                        % (len(self.trad_only), len(self.simp_only)))
        for key, rec in self.schemas.items():
            tag = rec.get("tag")
            if tag != "und" and tag not in self.known_tags:
                errs.append("schemas[%s].tag=%s 不在 languages 表裡" % (key, tag))
            if not rec.get("why"):
                errs.append("schemas[%s] 沒有寫理由（why）" % key)
            if tag == "und" and not rec.get("not_a_language"):
                errs.append("schemas[%s] 標 und 卻沒有 not_a_language：und 只有"
                            "「本來就不是語言」才該人工指定" % key)
        for key, rec in self.packages.items():
            if not rec.get("why"):
                errs.append("packages[%s] 沒有寫理由（why）" % key)
            if not rec.get("lang") and not rec.get("region"):
                errs.append("packages[%s] 既沒有 lang 也沒有 region" % key)
        for key, rec in self.rppi_paths.items():
            if not rec.get("lang") or not rec.get("why"):
                errs.append("rppi_paths[%s] 缺 lang 或 why" % key)
        seen = set()
        for l in self.languages:
            if l["tag"] in seen:
                errs.append("languages 有重複的 tag：%s" % l["tag"])
            seen.add(l["tag"])
        return errs

    def curated_schema(self, schema_uid, schema_id):
        """uid 優先於裸 id。回 (rec, 用到的鍵) 或 (None, None)。"""
        if schema_uid in self.schemas:
            return self.schemas[schema_uid], schema_uid
        if schema_id in self.schemas:
            return self.schemas[schema_id], schema_id
        return None, None


_data_cache = {}


def load_data(path=None):
    path = path or DATA_FILE
    if path in _data_cache:
        return _data_cache[path]
    import yaml
    with open(path, encoding="utf-8") as f:
        doc = yaml.safe_load(f)
    d = LangData(doc)
    errs = d.validate()
    if errs:
        raise SystemExit("語言資料檔 %s 有問題：\n  - %s" % (path, "\n  - ".join(errs)))
    _data_cache[path] = d
    return d


# ── 字集探針 ────────────────────────────────────────────────────────────────

def probe_text(data, text):
    t = s = 0
    for ch in text:
        if ch in data.trad_only:
            t += 1
        elif ch in data.simp_only:
            s += 1
    return t, s


def probe_files(data, paths):
    t = s = 0
    for p in paths:
        try:
            with open(p, encoding="utf-8", errors="ignore") as f:
                a, b = probe_text(data, f.read(data.read_bytes))
        except OSError:
            continue
        t += a
        s += b
    return t, s


def script_of(data, t, s):
    """(繁, 簡) → ("Hant"|"Hans"|None, 依據字串)。判不出來回 None。"""
    if t + s < data.min_sample:
        return None, "字集探針樣本不足（繁 %d / 簡 %d）" % (t, s)
    if t >= s * data.ratio:
        return "Hant", "字集探針：繁體專用字 %d vs 簡體專用字 %d" % (t, s)
    if s >= t * data.ratio:
        return "Hans", "字集探針：簡體專用字 %d vs 繁體專用字 %d" % (s, t)
    return None, "字集探針無定論（繁 %d / 簡 %d）" % (t, s)


# ── 套件目錄與 schema.yaml ─────────────────────────────────────────────────

def clone_dir(work, pkg):
    """analyze/pack 用的上游 clone 目錄。命名見 build_schema_store.sh。"""
    for cand in (pkg.get("repo", "").replace("/", "__"),
                 os.path.basename(pkg.get("upstream", ""))):
        if cand:
            d = os.path.join(work, "upstream", cand)
            if os.path.isdir(d):
                return d
    return None


def find_file(root, name):
    hits = glob.glob(os.path.join(root, name)) or \
        glob.glob(os.path.join(root, "*", name)) or \
        glob.glob(os.path.join(root, "*", "*", name))
    return hits[0] if hits else None


def schema_dicts(root, schema_id):
    """解析 <schema_id>.schema.yaml 的 translator/dictionary，回傳詞庫檔清單。

    刻意不做完整 YAML 解析：schema.yaml 大量使用 `__include` / `__patch`，
    PyYAML 讀得進來但語義要 librime 才展得開。這裡只要「哪一份詞庫」，
    正規表示式足夠，而且對壞檔案容忍度更高。
    """
    f = find_file(root, schema_id + ".schema.yaml")
    if not f:
        return [], None
    try:
        txt = open(f, encoding="utf-8", errors="ignore").read()
    except OSError:
        return [], None
    m = re.search(r"^\s*dictionary:\s*([A-Za-z0-9_.-]+)", txt, re.M)
    names = [m.group(1)] if m else []
    out = []
    for n in names:
        p = find_file(root, n + ".dict.yaml")
        if p:
            out.append(p)
            try:
                head = open(p, encoding="utf-8", errors="ignore").read(4000)
            except OSError:
                head = ""
            # import_tables 讓一份詞庫拆成好幾個檔
            for tbl in re.findall(r"^\s*-\s*([A-Za-z0-9_.-]+)\s*$", head, re.M):
                q = find_file(root, tbl + ".dict.yaml")
                if q and q not in out:
                    out.append(q)
    return out, txt


def all_dicts(root):
    out = glob.glob(os.path.join(root, "*.dict.yaml"))
    if not out:
        out = glob.glob(os.path.join(root, "*", "*.dict.yaml"))
    return out


# ── 主判定 ──────────────────────────────────────────────────────────────────

class Resolver(object):
    def __init__(self, work, packages, data=None):
        self.work = work
        self.data = data or load_data()
        self.pkgs = {p["id"]: p for p in packages}
        self._probe_cache = {}

    # 套件的字集：先看方案自己的詞庫，沒有就看套件的全部詞庫，
    # 再沒有就看它 requires 的套件（注音／雙拼借的是 luna_pinyin 的詞庫）。
    def package_script(self, pkg_id, schema_id=None, _depth=0):
        key = (pkg_id, schema_id)
        if key in self._probe_cache:
            return self._probe_cache[key]
        d = self.data
        pkg = self.pkgs.get(pkg_id)
        result = (None, "找不到套件 %s" % pkg_id)
        if pkg:
            root = clone_dir(self.work, pkg)
            if root:
                if schema_id:
                    paths, _ = schema_dicts(root, schema_id)
                    if paths:
                        sc, why = script_of(d, *probe_files(d, paths))
                        if sc:
                            result = (sc, why + "（方案自己的詞庫 %s）"
                                      % os.path.basename(paths[0]))
                            self._probe_cache[key] = result
                            return result
                sc, why = script_of(d, *probe_files(d, all_dicts(root)))
                if sc:
                    result = (sc, why + "（套件 %s 的詞庫）" % pkg_id)
                    self._probe_cache[key] = result
                    return result
                result = (None, why)
            if _depth < 2:
                for dep in pkg.get("requires") or []:
                    sc, why = self.package_script(dep, None, _depth + 1)
                    if sc:
                        result = (sc, why + "（本套件自己沒有詞庫，借相依套件 %s 的）" % dep)
                        break
        self._probe_cache[key] = result
        return result

    def resolve(self, schema_id, schema_name, pkg_id, schema_uid=None):
        """→ {"language", "source", "rule", "why", "not_a_language"}。

        判不出來時 language = "und"、source = "unknown"。
        """
        d = self.data
        schema_uid = schema_uid or (uidlib.make_uid(pkg_id, schema_id)
                                    if pkg_id else uidlib.builtin_uid(schema_id))

        def out(tag, kinds, rules, why, not_lang=False):
            return {"language": tag, "source": _worst(kinds),
                    "rule": "+".join(rules), "why": why,
                    "not_a_language": bool(not_lang)}

        # 0. 人工判定
        rec, key = d.curated_schema(schema_uid, schema_id)
        if rec:
            rule = "curated-schema-uid" if key == schema_uid else "curated-schema"
            return out(rec["tag"], ["curated"], [rule],
                       "人工判定（%s）：%s" % (key, rec["why"]),
                       rec.get("not_a_language"))

        pkg = self.pkgs.get(pkg_id) or {}
        kinds, rules = [], []

        # 1. 語言 subtag
        rppi = pkg.get("rppi_path")
        prec = d.packages.get(pkg_id) or {}
        if rppi in d.rppi_paths:
            lang = d.rppi_paths[rppi]["lang"]
            why = d.rppi_paths[rppi]["why"]
            kinds.append("upstream")
            rules.append("rppi-path")
        elif prec.get("lang"):
            lang, why = prec["lang"], prec["why"]
            kinds.append("curated")
            rules.append("curated-package")
        else:
            return out("und", ["unknown"], ["no-language-source"],
                       "rppi 未收錄且無人工對照，語言判不出來")

        if lang in ("ja", "ko", "en"):
            return out(lang, kinds, rules, why)

        # 2a. 方案名裡的字集／地區字樣
        for mm in d.name_marks:
            if mm["mark"] in (schema_name or ""):
                kinds.append("derived")
                rules.append("schema-name-mark")
                if lang == "zh":
                    return out(mm["tag"], kinds, rules,
                               why + "；方案名含「%s」" % mm["mark"])
                # 非華語的漢語支系只借字集，不借 zh
                script = mm["tag"].split("-")[1]
                return out("%s-%s" % (lang, script), kinds, rules,
                           why + "；方案名含「%s」" % mm["mark"])

        # 2b/2c. 字集探針
        script, probe_why = self.package_script(pkg_id, schema_id)
        if not script:
            return out("und", kinds + ["unknown"], rules + ["charset-undetermined"],
                       why + "；但字集判不出來（%s）" % probe_why)
        kinds.append("derived")
        rules.append("charset-probe")

        region = ""
        if prec.get("region") and script == "Hant":
            region = prec["region"]
            probe_why += "；地區依據：" + prec["why"]
            kinds.append("curated")
            rules.append("curated-package-region")
        tag = "-".join(x for x in (lang, script, region) if x)
        if tag not in d.known_tags:
            # 沒在表裡的組合寧可退到不帶地區的形式，也不要生出 app 不認得的標記
            fallback = "%s-%s" % (lang, script)
            if fallback in d.known_tags:
                return out(fallback, kinds, rules + ["fallback-drop-region"],
                           why + "；" + probe_why +
                           "（%s 不在語言表內，退回 %s）" % (tag, fallback))
            return out("und", kinds + ["unknown"], rules + ["tag-not-in-table"],
                       why + "；" + probe_why + "（%s 不在語言表內）" % tag)
        return out(tag, kinds, rules, why + "；" + probe_why)


# ── 報告與資產產生 ──────────────────────────────────────────────────────────

def build(work, only_pkgs=None, schemas_of=None, data=None):
    """→ (Resolver, rows, builtin)。

    `only_pkgs`   只看這些套件（mkindex 傳的是通過品質閘門的那一批）。
    `schemas_of`  pkg_id → 可用方案 id 集合（同上，過濾掉沒通過部署的方案）。
    不給就退回 packages.json 上一次留下的 `index_ids`，讓這支能單獨跑報告。

    每一列都帶 `uid`。**下游一律用 uid 當鍵**，裸的 schema id 只供顯示與相容。
    """
    d = data or load_data()
    doc = json.load(open(os.path.join(work, "packages.json")))
    if only_pkgs is None:
        only_pkgs = set(doc.get("index_ids") or [p["id"] for p in doc["packages"]])
    r = Resolver(work, doc["packages"], data=d)
    rows = []
    for p in sorted(doc["packages"], key=lambda x: x["id"]):
        if p["id"] not in only_pkgs:
            continue
        allowed = schemas_of.get(p["id"]) if schemas_of else None
        for sch in p.get("schemas") or []:
            if allowed is not None and sch["id"] not in allowed:
                continue
            u = uidlib.make_uid(p["id"], sch["id"])
            got = r.resolve(sch["id"], sch.get("name") or "", p["id"], schema_uid=u)
            row = {"uid": u, "schema": sch["id"], "name": sch.get("name") or "",
                   "package": p["id"]}
            row.update(got)
            rows.append(row)
    builtin = []
    for b in d.builtin:
        u = uidlib.builtin_uid(b["id"])
        got = r.resolve(b["id"], b.get("name") or "", b.get("package") or "",
                        schema_uid=u)
        rec = {"uid": u, "id": b["id"], "name": b.get("name") or b["id"],
               "package": uidlib.BUILTIN_NS}
        rec.update(got)
        builtin.append(rec)
    return r, rows, builtin


def flatten(rows, builtin):
    """→ (flat_by_bare_id, flat_by_uid, conflicts)。

    `flat_by_uid` 是**正確**的那一份：uid 全域唯一，不需要丟掉任何東西。

    `flat_by_bare_id` 只為了相容既有讀取端（Android 的 SchemaLanguages 目前
    以裸 id 查表）。判定**不同**的撞號 id 一律不寫進去：寫錯一半比留給執行期的
    InstalledRegistry（它知道是哪個套件裝的）去查要糟得多。

    ⚠ 判定**相同**的撞號 id（`pinyin_simp`、`radical_pinyin` 兩邊都是 zh-Hans）
    照收。它們的詞庫確實不同，但這張表回答的問題只有「這個方案是什麼語言」，
    而那個答案沒有歧義。為了「內容不同」把一個確定的正確答案丟掉，只會讓
    讀取端退回字面啟發式 —— 那才是真的會分錯。撞號的其他後果（佈局、已安裝
    判斷、覆蓋）由 uid 與索引的 conflicts 欄位承接，不歸這張表管。
    而且這條規則是自我修正的：哪天其中一邊換了字集，判定就會不同，
    這個 id 自動從表裡消失。
    """
    seen, conflicts = {}, {}
    for r0 in rows:
        sid, tag = r0["schema"], r0["language"]
        if sid in seen and seen[sid] != tag:
            conflicts.setdefault(sid, set()).update({seen[sid], tag})
        seen[sid] = tag
    flat = {sid: tag for sid, tag in seen.items()
            if tag != "und" and sid not in conflicts}
    by_uid = {r0["uid"]: r0["language"] for r0 in rows if r0["language"] != "und"}
    # 內建方案永遠勝出：它們隨 APK 出貨，語言是我們自己說了算的。
    for b in builtin:
        if b["language"] != "und":
            flat[b["id"]] = b["language"]
            by_uid[b["uid"]] = b["language"]
            conflicts.pop(b["id"], None)
    return flat, by_uid, {k: sorted(v) for k, v in conflicts.items()}


def coverage(rows, builtin):
    """→ 涵蓋率統計。「還有幾個方案是未知」是要能回答的問題。"""
    items = list(rows) + list(builtin)
    total = len(items)
    tagged = [i for i in items if i["language"] != "und"]
    not_lang = [i for i in items if i["language"] == "und" and i.get("not_a_language")]
    unknown = [i for i in items if i["language"] == "und" and not i.get("not_a_language")]
    by_source = {}
    for i in items:
        by_source[i["source"]] = by_source.get(i["source"], 0) + 1
    return {
        "total": total,
        "tagged": len(tagged),
        "not_a_language": len(not_lang),
        "unknown": len(unknown),
        # 分母刻意扣掉「本來就不是語言」的那些：它們標 und 是正確答案，
        # 算進未涵蓋只會讓數字永遠到不了 100%，然後就沒有人再看它。
        "coverage_pct": round(100.0 * len(tagged) / max(total - len(not_lang), 1), 1),
        "by_source": dict(sorted(by_source.items())),
        "unknown_items": [{"uid": i["uid"], "why": i["why"]} for i in unknown],
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description="給方案標上 BCP 47 語言標記")
    ap.add_argument("work", help="build/schema-store/_work")
    ap.add_argument("--asset", help="順便產生隨 APK 出貨的對照表 json")
    ap.add_argument("--report", help="把完整判定（含依據）寫成 json")
    ap.add_argument("--json", action="store_true", help="以 JSON 印出全部判定")
    ap.add_argument("--max-unknown", type=int, default=None,
                    help="未知（不含「本來就不是語言」）超過這個數目就 exit 1")
    args = ap.parse_args(argv)

    data = load_data()
    _, rows, builtin = build(args.work, data=data)
    flat, by_uid, conflicts = flatten(rows, builtin)
    cov = coverage(rows, builtin)

    if args.json:
        print(json.dumps({"schemas": rows, "builtin": builtin, "flat": flat,
                          "by_uid": by_uid, "conflicts": conflicts,
                          "coverage": cov}, ensure_ascii=False, indent=1))
        return 0

    print("方案筆數 %d（市集 %d + 內建 %d；同一個 id 出現在兩個套件算兩筆）"
          % (cov["total"], len(rows), len(builtin)))
    print("有語言標記 %d；本來就不是語言 %d；**未知 %d**；涵蓋率 %.1f%%"
          % (cov["tagged"], cov["not_a_language"], cov["unknown"], cov["coverage_pct"]))
    print("來源分佈：" + "、".join("%s %d" % kv for kv in cov["by_source"].items()))
    print("uid 對照表 %d 筆；裸 id 扁平表 %d 筆（判定不同的 %d 個撞號 id 不收）\n"
          % (len(by_uid), len(flat), len(conflicts)))

    order = {l["tag"]: l["order"] for l in data.languages}
    name_of = {l["tag"]: l["name"] for l in data.languages}
    by_tag = {}
    for i in list(rows) + list(builtin):
        by_tag.setdefault(i["language"], []).append(i)
    for tag in sorted(by_tag, key=lambda t: (order.get(t, 98), t)):
        items = by_tag[tag]
        print("%-12s %-16s %3d  %s" % (tag, name_of.get(tag, "?"), len(items),
                                       " ".join(sorted({i["uid"] for i in items}))))
    if conflicts:
        print("\n同 id 不同套件、判定不同（裸 id 扁平表不收，讀取端請改用 uid）：")
        for sid, tags in sorted(conflicts.items()):
            print("  %-26s %s" % (sid, " / ".join(tags)))
    if cov["unknown"]:
        print("\n未知清單（判不出來 —— 這是缺口，要補資料）：")
        for i in cov["unknown_items"]:
            print("  %-40s %s" % (i["uid"], i["why"]))
    nl = [i for i in list(rows) + list(builtin)
          if i["language"] == "und" and i.get("not_a_language")]
    if nl:
        print("\n標 und 但**不是缺口**（資料裡明寫 not_a_language）：")
        for i in sorted(nl, key=lambda x: x["uid"]):
            print("  %-40s %s" % (i["uid"], i["why"]))

    if args.asset:
        write_asset(args.asset, flat, by_uid)
        print("\n對照表已寫入 %s（裸 id %d 筆 / uid %d 筆）"
              % (args.asset, len(flat), len(by_uid)))
    if args.report:
        write_report(args.report, rows, builtin, cov, conflicts)
        print("完整判定依據已寫入 %s" % args.report)

    if args.max_unknown is not None and cov["unknown"] > args.max_unknown:
        print("\n錯誤：未知 %d 個，超過上限 %d" % (cov["unknown"], args.max_unknown),
              file=sys.stderr)
        return 1
    return 0


def load_dangerous_ids(path=None):
    """known_collisions.yaml 裡 severity=content-differs 的 schema id。"""
    import yaml
    path = path or os.path.join(DATA_DIR, "known_collisions.yaml")
    if not os.path.exists(path):
        return set()
    with open(path, encoding="utf-8") as f:
        doc = yaml.safe_load(f) or {}
    return {c["schema"] for c in (doc.get("schema_ids") or [])
            if c.get("severity") == "content-differs"}


def _now():
    return datetime.datetime.now(datetime.timezone.utc) \
        .replace(microsecond=0).isoformat().replace("+00:00", "Z")


def write_asset(path, flat, by_uid):
    """隨 APK 出貨的對照表。

    為什麼要有這一份而不是只靠索引：索引是下載來的，可能比 app 舊、
    也可能根本還沒下載過。內建的四個方案又不在索引的 packages 裡。
    這一份跟著 APK 走，永遠與內建方案同版本。

    ⚠ `format_version` 保持 1：現行 Android 的 SchemaLanguages.parse 是
    `format_version != 1 → 整份丟掉`。新增的 `schemas_by_uid` 是**加欄位**，
    舊讀取端看不到它、行為完全不變。相容性規則見 docs/schema-store.md §1.1。
    """
    data = load_data()
    doc = {
        "format_version": FORMAT_VERSION,
        "format_minor": FORMAT_MINOR,
        "generated_at": _now(),
        "languages": data.languages,
        # 舊讀取端讀這個（裸 id，撞號的不收）
        "schemas": dict(sorted(flat.items())),
        # 新讀取端讀這個（uid，完整、無歧義）
        "schemas_by_uid": dict(sorted(by_uid.items())),
    }
    d = os.path.dirname(os.path.abspath(path))
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
        f.write("\n")


def write_report(path, rows, builtin, cov, conflicts):
    """完整的判定依據。**這是「可追溯」的落腳處。**

    索引裡只放 `language` 與 `language_source`（一個字的來源分級），
    完整的理由字串放這裡；它會跟索引一起上傳，任何人都可以逐條複查
    「這個標記是上游說的，還是你們自己標的，依據是什麼」。
    """
    doc = {
        "format_version": FORMAT_VERSION,
        "format_minor": FORMAT_MINOR,
        "generated_at": _now(),
        "coverage": cov,
        "collisions": conflicts,
        "schemas": [
            {"uid": r["uid"], "package": r["package"], "schema": r["schema"],
             "name": r["name"], "language": r["language"],
             "source": r["source"], "rule": r["rule"], "why": r["why"],
             "not_a_language": r.get("not_a_language", False)}
            for r in sorted(rows, key=lambda x: x["uid"])],
        "builtin": [
            {"uid": b["uid"], "schema": b["id"], "name": b["name"],
             "language": b["language"], "source": b["source"],
             "rule": b["rule"], "why": b["why"],
             "not_a_language": b.get("not_a_language", False)}
            for b in builtin],
    }
    d = os.path.dirname(os.path.abspath(path))
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
        f.write("\n")


if __name__ == "__main__":
    sys.exit(main())
