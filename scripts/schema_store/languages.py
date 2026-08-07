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

判定順序（規範性；每一層都把依據寫進 `source`）
─────────────────────────────────────────────────────────────────────────────
  0. MANUAL[schema_id]  —— 人工判定，附理由。只用在自動規則會判錯的地方：
       工具方案（不產出文字）、套件裡的異語言子方案、地區只有人看得出來的。
  1. 語言 subtag：rppi 的分類路徑（`Chinese/Cantonese` → `yue`…）。
       rppi 沒收錄的套件走 PKG_LANG（人工，附理由）。
  2. script / region：
       a. 方案名或 schema.yaml 的 description 裡的地區字樣
          （臺灣正體 / 简化字 / 香港 …）；
       b. **字集探針**：解析該方案自己的 `translator/dictionary`，
          把對應的 `*.dict.yaml` 拿來數「繁體專用字」與「簡體專用字」。
          比例 >= 2:1 且樣本 >= MIN_SAMPLE 才算數。
       c. 自己沒有詞庫時（注音、雙拼、宮保這類借別人詞庫的方案），
          改探針該套件 `requires` 的套件 —— 借誰的詞庫就跟誰同字集。
  3. 以上都判不出來 → `und`。**不猜。**

`und` 的兩種來源在報告裡是分開的，因為處置方式不同：
  · 「不是語言」（IPA 音標、工具方案）—— 標 `und` 是正確答案，不必修；
  · 「查不到」—— 是缺口，要補資料。

用法
    python3 languages.py <WORK>            # 印出報告（涵蓋率、und 清單）
    python3 languages.py <WORK> --asset <out.json>   # 產生隨 APK 出貨的對照表
"""

from __future__ import annotations

import argparse
import datetime
import glob
import json
import os
import re
import sys

FORMAT_VERSION = 1

# ── 語言標記表（BCP 47）─────────────────────────────────────────────────────
# order 決定選單裡的分組順序。name 是 zh-Hant-TW 語系下的顯示名，
# 與 ConfigRepository.LOCALE 一致 —— 分組標題不該在 app 裡再寫死一份。
LANGUAGES = [
    {"tag": "zh-Hant-TW", "name": "中文（臺灣正體）", "order": 1},
    {"tag": "zh-Hant-HK", "name": "中文（香港繁體）", "order": 2},
    {"tag": "zh-Hant",    "name": "中文（繁體）",     "order": 3},
    {"tag": "zh-Hans",    "name": "中文（简体）",     "order": 4},
    {"tag": "yue-Hant-HK", "name": "粵語（香港）",    "order": 10},
    {"tag": "yue-Hant-CN", "name": "粵語（廣西）",    "order": 11},
    {"tag": "yue-Hant",   "name": "粵語",            "order": 12},
    {"tag": "nan-Hant",   "name": "閩南語",          "order": 20},
    {"tag": "wuu-Hant",   "name": "吳語",            "order": 21},
    {"tag": "hsn-Hant",   "name": "湘語",            "order": 22},
    {"tag": "hsn-Cyrl",   "name": "湘語（西里爾轉寫）", "order": 23},
    {"tag": "ltc-Hant",   "name": "中古漢語",        "order": 30},
    {"tag": "ja",         "name": "日本語",          "order": 40},
    {"tag": "ko",         "name": "한국어",           "order": 41},
    {"tag": "en",         "name": "English",         "order": 50},
    {"tag": "und",        "name": "其他（未標記語言）", "order": 99},
]
KNOWN_TAGS = {l["tag"] for l in LANGUAGES}

# ── 第 1 層：rppi 分類路徑 → 語言 subtag ───────────────────────────────────
# 依據：rppi（rime/rppi）的 index.json 把每個 recipe 掛在一條分類路徑下，
# 那條路徑是上游維護者對「這是哪一種漢語」的判斷，比我們自己猜可靠。
# 注意 `Chinese/Shape`（形碼）與 `Chinese/Mandarin`（拼音）都是華語 ——
# 那條軸是輸入方式，不是語言，所以兩者都對到 zh。
RPPI_LANG = {
    "Chinese/Mandarin":  ("zh",  "rppi 分類路徑 Chinese/Mandarin"),
    "Chinese/Shape":     ("zh",  "rppi 分類路徑 Chinese/Shape（形碼仍是華語）"),
    "Chinese/Cantonese": ("yue", "rppi 分類路徑 Chinese/Cantonese"),
    "Chinese/Wu":        ("wuu", "rppi 分類路徑 Chinese/Wu"),
    "Chinese/Hokkien":   ("nan", "rppi 分類路徑 Chinese/Hokkien"),
    "Chinese/Xiang":     ("hsn", "rppi 分類路徑 Chinese/Xiang"),
    "Chinese/Old":       ("ltc", "rppi 分類路徑 Chinese/Old（中古漢語）"),
    "Chinese/Hakka":     ("hak", "rppi 分類路徑 Chinese/Hakka"),
    "Collection":        ("zh",  "rppi 分類路徑 Collection（整合包，主體為華語）"),
    "Japanese":          ("ja",  "rppi 分類路徑 Japanese"),
    "Korean":            ("ko",  "rppi 分類路徑 Korean"),
    "English":           ("en",  "rppi 分類路徑 English"),
}

# rppi 沒收錄的套件。只寫語言 subtag，script/region 仍走探針。
PKG_LANG = {
    "jyutping": ("yue", "上游 rime/rime-jyutping：粵拼／耶魯／香港廣東話拼音"),
    "hangul":   ("ko",  "上游 picado-tv/rime-hangul：韓文字母組字"),
    "handarin": ("zh",  "上游 README：給韓語母語者的華語拼音方案"),
}

# ── 第 0 層：人工判定 ───────────────────────────────────────────────────────
# 只在自動規則會判錯時出現，每一筆都要寫得出理由。
# 三類：(a) 不產出自然語言文字的工具／音標方案；(b) 套件裡的異語言子方案；
#      (c) 地區只有從方案名／README 看得出來的。
MANUAL = {
    # (a) 不是語言 —— 標 und 是正確答案，不是缺口
    "ipa_xsampa":  ("und", "X-SAMPA 音標轉寫，不對應任何特定語言"),
    "ipa_yunlong": ("und", "雲龍國際音標轉寫，不對應任何特定語言"),
    "moran_charset": ("und", "上游方案名標為「勿用·工具方案」，不供使用者輸入"),
    "moran_reverse": ("und", "上游方案名標為「勿用·工具方案」，反查用"),
    "array30_query": ("und", "行列30 萬用字元查詢，工具方案"),

    # (b) 套件裡的異語言子方案
    "moran_english": ("en", "萬象整合包內的英文子方案"),
    "melt_eng":      ("en", "雾凇整合包內的 Easy English Nano 英文子方案"),
    "moran_japanese": ("ja", "萬象整合包內的「簡易日語」子方案"),

    # (c) 地區／字集寫在方案名上
    "luna_pinyin_tw": ("zh-Hant-TW", "方案名「朙月拼音·臺灣正體」；schema 以 opencc 切臺灣字形"),
    "bopomofo_tw":    ("zh-Hant-TW", "方案名「注音·臺灣正體」"),
    "luna_pinyin_simp": ("zh-Hans", "方案名「朙月拼音·简化字」"),
    "hkcantonese":    ("yue-Hant-HK", "方案名「香港廣東話拼音」"),
    "sautungva_cyrillic": ("hsn-Cyrl", "方案名「邵東話西里爾」：輸出西里爾轉寫"),
    "wubi_trad":      ("zh-Hant", "方案名「五筆·簡入繁出」：輸入簡碼、輸出繁體"),
    "wubi86_jidian_trad": ("zh-Hant", "方案名「简入繁出」：輸出繁體"),
    "wubi86_jidian_trad_pinyin": ("zh-Hant", "方案名「拼音混输繁体」：輸出繁體"),

    # 內建方案（不在市集索引裡，見 core/data/shared/）
    "t9_pinyin": ("zh-Hant", "本專案自撰，共用 luna_pinyin 的繁體詞庫"
                             "（core/data/shared/t9_pinyin.schema.yaml）"),
}

# 套件層級的地區斷言：整包適用，但仍讓 MANUAL 的個別方案覆寫。
PKG_REGION = {
    "cantonese":         ("HK", "上游 rime/rime-cantonese README 以香港粵語為對象"),
    "naamning-jyutping": ("CN", "方案名指名南寧（廣西），屬中國大陸"),
}

# ── 字集探針 ────────────────────────────────────────────────────────────────
# 成對且無歧義的繁／簡專用字。刻意用「對照組」而不是單邊清單：
# 兩邊同形的字（人、山、日）對判別毫無貢獻，只會稀釋樣本。
_TRAD = ("們這對將來時間國學說話點沒開關門問題經過為體實現發現當學習語言鄉黨屬醫藥"
         "親愛聽見覺得應該讓總會運動車輛買賣錢財書寫讀個處無與後從東西馬鳥魚龍鳳歲"
         "萬億業產權義務廣場園區樓層機構標準價格網絡電視聲響顯示齒輪飛機藝術傳統節"
         "慶禮儀衛生檢查驗證豐富華麗準備繼續斷絕舊識懷疑階級鬥爭觀樂園報紙製造義務")
_SIMP = ("们这对将来时间国学说话点没开关门问题经过为体实现发现当学习语言乡党属医药"
         "亲爱听见觉得应该让总会运动车辆买卖钱财书写读个处无与后从东西马鸟鱼龙凤岁"
         "万亿业产权义务广场园区楼层机构标准价格网络电视声响显示齿轮飞机艺术传统节"
         "庆礼仪卫生检查验证丰富华丽准备继续断绝旧识怀疑阶级斗争观乐园报纸制造义务")
TRAD_ONLY = set(_TRAD) - set(_SIMP)
SIMP_ONLY = set(_SIMP) - set(_TRAD)

MIN_SAMPLE = 50      # 樣本太小的比例沒有意義
RATIO = 2.0          # 一邊要是另一邊的兩倍以上才敢下判斷
READ_BYTES = 600000  # 詞庫動輒數 MB，取前段就夠分辨字集


def probe_text(text):
    t = s = 0
    for ch in text:
        if ch in TRAD_ONLY:
            t += 1
        elif ch in SIMP_ONLY:
            s += 1
    return t, s


def probe_files(paths):
    t = s = 0
    for p in paths:
        try:
            with open(p, encoding="utf-8", errors="ignore") as f:
                a, b = probe_text(f.read(READ_BYTES))
        except OSError:
            continue
        t += a
        s += b
    return t, s


def script_of(t, s):
    """(繁, 簡) → ("Hant"|"Hans"|None, 依據字串)。判不出來回 None。"""
    total = t + s
    if total < MIN_SAMPLE:
        return None, "字集探針樣本不足（繁 %d / 簡 %d）" % (t, s)
    if t >= s * RATIO:
        return "Hant", "字集探針：繁體專用字 %d vs 簡體專用字 %d" % (t, s)
    if s >= t * RATIO:
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
    # import_tables 讓一份詞庫拆成好幾個檔
    for mm in re.finditer(r"^\s*-\s*([A-Za-z0-9_.-]+)\s*$", txt, re.M):
        pass
    out = []
    for n in names:
        p = find_file(root, n + ".dict.yaml")
        if p:
            out.append(p)
            try:
                head = open(p, encoding="utf-8", errors="ignore").read(4000)
            except OSError:
                head = ""
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

REGION_MARKS = [
    ("臺灣正體", "zh-Hant-TW"), ("台灣正體", "zh-Hant-TW"),
    ("臺灣", "zh-Hant-TW"), ("台灣", "zh-Hant-TW"),
    ("香港", "zh-Hant-HK"),
    # 「简化字／简体」講的是**字集**不是地區，所以只給 script，不擅自補 -CN。
    ("简化字", "zh-Hans"), ("簡化字", "zh-Hans"),
    ("简体", "zh-Hans"), ("簡體", "zh-Hans"),
]


class Resolver(object):
    def __init__(self, work, packages):
        self.work = work
        self.pkgs = {p["id"]: p for p in packages}
        self._probe_cache = {}

    # 套件的字集：先看方案自己的詞庫，沒有就看套件的全部詞庫，
    # 再沒有就看它 requires 的套件（注音／雙拼借的是 luna_pinyin 的詞庫）。
    def package_script(self, pkg_id, schema_id=None, _depth=0):
        key = (pkg_id, schema_id)
        if key in self._probe_cache:
            return self._probe_cache[key]
        pkg = self.pkgs.get(pkg_id)
        result = (None, "找不到套件 %s" % pkg_id)
        if pkg:
            root = clone_dir(self.work, pkg)
            if root:
                paths = []
                if schema_id:
                    paths, _ = schema_dicts(root, schema_id)
                    if paths:
                        sc, why = script_of(*probe_files(paths))
                        if sc:
                            result = (sc, why + "（方案自己的詞庫 %s）"
                                      % os.path.basename(paths[0]))
                            self._probe_cache[key] = result
                            return result
                sc, why = script_of(*probe_files(all_dicts(root)))
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

    def resolve(self, schema_id, schema_name, pkg_id):
        """→ (tag, source)。判不出來回 ("und", 理由)。"""
        # 0. 人工判定
        if schema_id in MANUAL:
            tag, why = MANUAL[schema_id]
            return tag, "人工判定：" + why
        pkg = self.pkgs.get(pkg_id) or {}

        # 1. 語言 subtag
        rppi = pkg.get("rppi_path")
        if rppi in RPPI_LANG:
            lang, why = RPPI_LANG[rppi]
        elif pkg_id in PKG_LANG:
            lang, why = PKG_LANG[pkg_id]
        else:
            return "und", "rppi 未收錄且無人工對照，語言判不出來"

        if lang in ("ja", "ko", "en"):
            return lang, why

        # 2a. 方案名裡的字集／地區字樣
        for mark, tag in REGION_MARKS:
            if mark in (schema_name or ""):
                if lang == "zh":
                    return tag, why + "；方案名含「%s」" % mark
                # 非華語的漢語支系只借字集，不借 zh
                script = tag.split("-")[1]
                return "%s-%s" % (lang, script), why + "；方案名含「%s」" % mark

        # 2b/2c. 字集探針
        script, probe_why = self.package_script(pkg_id, schema_id)
        if not script:
            return "und", why + "；但字集判不出來（%s）" % probe_why

        region = ""
        if pkg_id in PKG_REGION and script == "Hant":
            region, region_why = PKG_REGION[pkg_id]
            probe_why += "；地區依據：" + region_why
        tag = "-".join(x for x in (lang, script, region) if x)
        if tag not in KNOWN_TAGS:
            # 沒在表裡的組合寧可退到不帶地區的形式，也不要生出 app 不認得的標記
            fallback = "%s-%s" % (lang, script)
            if fallback in KNOWN_TAGS:
                return fallback, why + "；" + probe_why + \
                    "（%s 不在語言表內，退回 %s）" % (tag, fallback)
            return "und", why + "；" + probe_why + "（%s 不在語言表內）" % tag
        return tag, why + "；" + probe_why


# ── 報告與資產產生 ──────────────────────────────────────────────────────────

# 內建方案（隨 APK 出貨，不在市集索引的 packages 裡）。
# 來源：core/data/user/default.custom.yaml 的 schema_list，
# 由 store/BuiltinMigration.kt 在升級時消費。
BUILTIN = [
    ("luna_pinyin_tw", "朙月拼音·臺灣正體", "luna-pinyin"),
    ("bopomofo_tw",    "注音·臺灣正體",     "bopomofo"),
    ("luna_pinyin",    "朙月拼音",          "luna-pinyin"),
    ("t9_pinyin",      "九宮格拼音",        None),
]


def build(work, only_pkgs=None, schemas_of=None):
    """→ (Resolver, rows, builtin)。

    `only_pkgs`   只看這些套件（mkindex 傳的是通過品質閘門的那一批）。
    `schemas_of`  pkg_id → 可用方案 id 集合（同上，過濾掉沒通過部署的方案）。
    不給就退回 packages.json 上一次留下的 `index_ids`，讓這支能單獨跑報告。
    """
    data = json.load(open(os.path.join(work, "packages.json")))
    if only_pkgs is None:
        only_pkgs = set(data.get("index_ids") or [p["id"] for p in data["packages"]])
    r = Resolver(work, data["packages"])
    rows = []
    for p in sorted(data["packages"], key=lambda x: x["id"]):
        if p["id"] not in only_pkgs:
            continue
        allowed = schemas_of.get(p["id"]) if schemas_of else None
        for sch in p.get("schemas") or []:
            if allowed is not None and sch["id"] not in allowed:
                continue
            tag, src = r.resolve(sch["id"], sch.get("name") or "", p["id"])
            rows.append({"schema": sch["id"], "name": sch.get("name") or "",
                         "package": p["id"], "language": tag, "source": src})
    builtin = []
    for sid, name, pid in BUILTIN:
        tag, src = r.resolve(sid, name, pid or "")
        builtin.append({"id": sid, "name": name, "language": tag, "source": src})
    return r, rows, builtin


def flatten(rows, builtin):
    """schema id → tag 的扁平對照表，外加撞號清單。

    ⚠ **方案 id 不是全域唯一的。** `double_pinyin` 同時存在於 `double-pinyin`
    （借朙月拼音的繁體詞庫）與 `ice`（簡體詞庫）兩個套件裡，字集相反。
    扁平表沒有辦法同時表達兩者，所以撞號且判定不同的 id **一律不寫進表裡**：
    寫錯一半，比留給執行期的 InstalledRegistry（它知道是哪個套件裝的）去查
    要糟得多。撞號清單會列在報告裡。
    """
    seen, conflicts = {}, {}
    for r0 in rows:
        sid, tag = r0["schema"], r0["language"]
        if sid in seen and seen[sid] != tag:
            conflicts.setdefault(sid, set()).update({seen[sid], tag})
        seen[sid] = tag
    flat = {sid: tag for sid, tag in seen.items()
            if tag != "und" and sid not in conflicts}
    # 內建方案永遠勝出：它們隨 APK 出貨，語言是我們自己說了算的。
    for b in builtin:
        if b["language"] != "und":
            flat[b["id"]] = b["language"]
            conflicts.pop(b["id"], None)
    return flat, {k: sorted(v) for k, v in conflicts.items()}


def main(argv=None):
    ap = argparse.ArgumentParser(description="給方案標上 BCP 47 語言標記")
    ap.add_argument("work", help="build/schema-store/_work")
    ap.add_argument("--asset", help="順便產生隨 APK 出貨的對照表 json")
    ap.add_argument("--json", action="store_true", help="以 JSON 印出全部判定")
    args = ap.parse_args(argv)

    _, rows, builtin = build(args.work)
    flat, conflicts = flatten(rows, builtin)
    if args.json:
        print(json.dumps({"schemas": rows, "builtin": builtin,
                          "flat": flat, "conflicts": conflicts},
                         ensure_ascii=False, indent=1))
        return 0

    by_tag = {}
    for r0 in rows + [{"language": b["language"], "schema": b["id"],
                       "name": b["name"], "package": "(內建)", "source": b["source"]}
                      for b in builtin]:
        by_tag.setdefault(r0["language"], []).append(r0)
    total = sum(len(v) for v in by_tag.values())
    und = by_tag.get("und", [])
    print("方案筆數 %d（市集 %d + 內建 %d；同一個 id 出現在兩個套件算兩筆）"
          % (total, len(rows), len(builtin)))
    print("有語言標記 %d（%.1f%%）；und %d（%.1f%%）"
          % (total - len(und), 100.0 * (total - len(und)) / total,
             len(und), 100.0 * len(und) / total))
    print("扁平對照表 %d 筆（撞號且判定不同的 %d 個 id 已排除）\n"
          % (len(flat), len(conflicts)))
    order = {l["tag"]: l["order"] for l in LANGUAGES}
    name_of = {l["tag"]: l["name"] for l in LANGUAGES}
    for tag in sorted(by_tag, key=lambda t: (order.get(t, 98), t)):
        items = by_tag[tag]
        print("%-12s %-16s %3d  %s" % (tag, name_of.get(tag, "?"), len(items),
                                       " ".join(sorted({i["schema"] for i in items}))))
    if conflicts:
        print("\n同 id 不同套件、判定不同（扁平表不收，執行期靠 InstalledRegistry 分辨）：")
        for sid, tags in sorted(conflicts.items()):
            print("  %-26s %s" % (sid, " / ".join(tags)))
    if und:
        print("\nund 清單（判不出來，或本來就不是語言）：")
        for i in sorted(und, key=lambda x: (x["package"], x["schema"])):
            print("  %-26s %-18s %s" % (i["schema"], i["package"], i["source"]))

    if args.asset:
        write_asset(args.asset, flat)
        print("\n對照表已寫入 %s（%d 筆）" % (args.asset, len(flat)))
    return 0


def write_asset(path, flat):
    """隨 APK 出貨的對照表。

    為什麼要有這一份而不是只靠索引：索引是下載來的，可能比 app 舊、
    也可能根本還沒下載過。內建的四個方案又不在索引的 packages 裡。
    這一份跟著 APK 走，永遠與內建方案同版本。
    """
    doc = {
        "format_version": FORMAT_VERSION,
        "generated_at": datetime.datetime.now(datetime.timezone.utc)
                        .replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "languages": LANGUAGES,
        "schemas": dict(sorted(flat.items())),
    }
    d = os.path.dirname(os.path.abspath(path))
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
        f.write("\n")


if __name__ == "__main__":
    sys.exit(main())
