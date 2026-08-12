#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_charset_data.py — 產生「字集守門」用的兩張字表與兩張補充轉換表

═══════════════════════════════════════════════════════════════════════════
 這支腳本產生什麼
═══════════════════════════════════════════════════════════════════════════

  core/data/lua/luminakey_charset_hans.lua   簡體字集（《通用规范汉字表》）
  core/data/lua/luminakey_charset_hant.lua   繁體字集（Big5）
  core/data/opencc/luminakey_t2s_extra.txt   簡體方向的補充轉換表
  core/data/opencc/luminakey_t2tw_extra.txt  臺灣字形方向的補充轉換表

前兩份被 core/data/lua/luminakey_charset.lua（librime 的 lua_filter）require
進去，決定「這個候選字在不在使用者選的那一種字裡」。後兩份掛在 opencc 的
conversion_chain 尾端，把**轉得掉的**先轉掉 —— 轉掉不損失候選，濾掉會。

═══════════════════════════════════════════════════════════════════════════
 判準是怎麼選的（這一段是規範性的，四端共用同一份結論）
═══════════════════════════════════════════════════════════════════════════

不能用 opencc 自己當判準，兩個方向都不行：

  · 正向：`妳` 在 opencc 全部 26 份 .txt 詞典裡一次都沒出現，所以 t2s
    永遠不會動它 —— 而它正是使用者回報的那一個字。
  · 反向：把「STCharacters 的 key」當成簡化字會誤殺。實跑 luna 詞庫：
    8.42% 詞條、加權 7.22% 被誤報，第一名是「了」。原因是它們對應到多個
    繁體形、其中包含自己。

所以改用兩個**外部**字集：

  簡體  《通用规范汉字表》8105 字（教育部／国家语委 2013）
        離線副本取自 rime-ice 打包的 cn_dicts/8105.dict.yaml。
        去重後 8,181 個漢字 + 一個「〇」（非漢字，不收）。
        ⚠ 比表名的 8,105 多 76 個 —— 上游那份檔案本來就多收了一些，
          這裡照實記錄，不假裝它剛好是 8105。
  繁體  Big5。CNS 系的事實標準，Python 的 codec 內建，不必下載，
        任何人在任何機器上都能重算出同一份。

**為什麼繁體不用 big5hkscs**（收 17,563 字，比 Big5 多 4,500）：實測它收了
`国`『学』『发』『个』『东』『业』『众』等 351 個真正的簡化字。用它當繁體
判準等於讓簡體字合法通過，那正是這件事要擋的。Big5 只漏 60 個，而那 60 個
（与 么 优 体 儿 凄 峰 床 并 庄 昵 晒 栖 气 …）多半在兩套字裡都通用。

**Big5 的代價要說清楚**：它缺 `酶` `礴` `珏` `堃` `喆` 這類字。選繁體時
它們會被濾掉。這是 Big5 這個 1984 年的字集本身的缺口，不是我們的判斷。

═══════════════════════════════════════════════════════════════════════════
 補充轉換表是怎麼算的
═══════════════════════════════════════════════════════════════════════════

補表掛在 conversion_chain 的**最後一段**，所以它的 key 是**前面那幾段轉完
之後**的字，不是詞庫裡的原字。腳本因此真的去跑一次 host 版 opencc：把詞庫
用到的每一個漢字送進 t2s.json / t2tw.json，拿轉換後的結果當 key 的候選。

對每一個「轉完仍在字集外」的字 C，依序找一個在字集內的替身 D：

  1. 日本新字体 → 舊字体（只在 C 既不是规范简化字、也不在 Big5 裡時才用 ——
     `芸` `缶` `弁` 這些在中文裡另有其字，不可以被日文對照表帶走）
  2. 臺／港字形的另一支（TWVariants / HKVariants 的兩個方向）
  3. 直接的簡繁對應（TSCharacters / STCharacters）
  4. C 是某個簡化字的繁體形（STCharacters 反查）
  5. 手工表（MANUAL_*）—— opencc 完全沒收的那些，`妳→你` 就在這裡

算出來的 D 會再送一次同一條 opencc 鏈做正規化，免得補表吐出一個
「在字集內、但不是這個地區慣用字形」的結果（例如 众→衆 應該是 众→眾）。

═══════════════════════════════════════════════════════════════════════════
 用法
═══════════════════════════════════════════════════════════════════════════

    scripts/gen_charset_data.py \\
        --opencc-src  third_party/librime/deps/opencc/data/dictionary \\
        --opencc-bin  third_party/build/host-opencc-install/bin/opencc \\
        --opencc-cfg  core/data/shared/opencc \\
        --hans-source <某份 8105.dict.yaml> \\
        --dict        core/data/shared/luna_pinyin.dict.yaml \\
        --essay       core/data/shared/essay.txt

`--hans-source` 只在要重建簡體字表時給；不給就沿用已經產生好的那一份
（字集本身不會因為詞庫變動而改變）。加 `--check` 只比對不寫檔，
用在「有人手改了產生出來的檔案」的守門。

⚠ 這支腳本**跑不動也沒關係**：產出的四個檔案是簽進 repo 的，它們才是
  事實來源。腳本在這裡是為了讓「這些字是怎麼來的」有一個能重跑的答案。
"""

import argparse
import collections
import hashlib
import io
import os
import subprocess
import sys

# ─────────────────────────────────────────────────────────────── 漢字範圍 ───
# 與 core/data/lua/luminakey_charset.lua 的 HAN_RANGES **必須一致**。
# 兩邊各寫一份是因為一邊是 Python 一邊是 Lua，沒有共用的地方；
# scripts/verify_charset_guard.sh 會比對這兩份清單的文字，改了一邊會紅。
HAN_RANGES = (
    (0x3400, 0x4DBF),    # 擴展 A
    (0x4E00, 0x9FFF),    # 基本區
    (0xF900, 0xFAFF),    # 相容漢字
    (0x20000, 0x3134F),  # 擴展 B 以上
)


def is_han(ch):
    o = ord(ch)
    return any(a <= o <= b for a, b in HAN_RANGES)


# ────────────────────────────────────────────────────────────── 手工補表 ───
# opencc 一條都沒有、而使用者真的會撞到的字。每一條都要寫得出理由。
#
# ⚠ 只放**意思相同、可以直接替換**的。像 `蚵`（蚵仔）、`砦`、`疋`、`糸`
#   這種「不在字表裡但也沒有別的寫法」的字**不放** —— 硬換成別的字是竄改，
#   那種字只能濾掉。
MANUAL_T2S = [
    ("妳", "你", "台港把第二人稱分性別寫；规范字表只有『你』"),
    ("牠", "它", "台港的動物第三人稱"),
    ("祂", "他", "台港的神明第三人稱"),
    ("伱", "你", "『你』的異體"),
    ("儞", "你", "『你』的異體"),
    ("尓", "尔", "『尔』的異體"),
    ("昰", "是", "『是』的異體"),
    ("碁", "棋", "『棋』的異體，日式寫法也用它"),
    ("峩", "峨", "『峨』的異體"),
    ("尅", "克", "『克』的異體"),
    ("衹", "只", "『只』的異體"),
    ("喫", "吃", "『吃』的舊寫"),
    ("皁", "皂", "『皂』的異體"),
    ("邨", "村", "『村』的異體"),
    ("湼", "涅", "『涅』的異體"),
    ("卻", "却", "台港字形；规范字表作『却』"),
    ("匃", "丐", "『丐』的異體"),
    ("嬭", "奶", "『奶』的繁體異寫"),
    ("麤", "粗", "『粗』的古字"),
    ("皃", "貌", "『貌』的古字"),
]

MANUAL_T2TW = [
    ("囯", "國", "『國』的俗寫"),
    ("旸", "暘", "簡化字，opencc 的 ST 沒收"),
]


# ──────────────────────────────────────────────────────────────── 讀檔 ───
def rime_dict_rows(path):
    """讀 RIME 的 .dict.yaml：`...` 之後才是資料。"""
    rows, started = [], False
    for line in io.open(path, encoding="utf-8"):
        if not started:
            if line.strip() == "...":
                started = True
            continue
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        rows.append(line.split("\t"))
    return rows


def opencc_pairs(path):
    """opencc 的 .txt：`key<TAB>值1 值2 …`，只留單字條目。"""
    m = collections.OrderedDict()
    for line in io.open(path, encoding="utf-8"):
        a = line.rstrip("\n").split("\t")
        if len(a) >= 2 and len(a[0]) == 1:
            m[a[0]] = [x for x in a[1].split(" ") if len(x) == 1]
    return m


def reverse(m):
    r = collections.OrderedDict()
    for k, vs in m.items():
        for v in vs:
            r.setdefault(v, []).append(k)
    return r


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


# ─────────────────────────────────────────────────────────────── 字集 ───
def big5_han():
    """Big5 收得下的漢字。任何有 Python 的機器都能重算出同一份。"""
    out = set()
    for a, b in HAN_RANGES:
        for cp in range(a, b + 1):
            ch = chr(cp)
            try:
                ch.encode("big5")
            except Exception:
                continue
            out.add(ch)
    return out


def hans_from_source(path):
    rows = rime_dict_rows(path)
    return {r[0] for r in rows if len(r[0]) == 1 and is_han(r[0])}


def hans_from_module(path):
    """從已經產生好的 .lua 資料模組把字讀回來（不重建時走這條）。"""
    text = io.open(path, encoding="utf-8").read()
    return {ch for ch in text if is_han(ch)}


# ────────────────────────────────────────────────────────── 產出格式 ───
LUA_HEADER = """\
-- {file}
--
-- **這個檔案是產生出來的，不要手改。** 產生器：scripts/gen_charset_data.py
--
-- {title}
-- 字數：{count}
-- 來源：{origin}
{extra}--
-- 格式：一個 Lua 長字串。讀它的 luminakey_charset.lua 只收漢字碼位，
-- 所以底下的換行不影響結果（純粹為了讓 diff 讀得動）。

return [[
"""


def write_lua_module(path, chars, title, origin, extra_lines, check):
    body = []
    ordered = sorted(chars, key=ord)
    for i in range(0, len(ordered), 64):
        body.append("".join(ordered[i:i + 64]))
    extra = "".join("-- %s\n" % line for line in extra_lines)
    text = LUA_HEADER.format(
        file=os.path.basename(path), title=title, count=len(ordered),
        origin=origin, extra=extra,
    ) + "\n".join(body) + "\n]]\n"
    return _emit(path, text, check)


def write_text_dict(path, table, title, notes, check):
    lines = ["# %s" % title,
             "#",
             "# **這個檔案是產生出來的，不要手改。** 產生器：scripts/gen_charset_data.py",
             "#"]
    lines += ["# %s" % n for n in notes]
    lines += ["#",
              "# opencc 的 text 型詞典：key<TAB>value，一行一條。",
              "# 它掛在 conversion_chain 的最後一段，所以 key 是**前面幾段轉完之後**的字。",
              "# 條數：%d" % len(table),
              ""]
    for k in sorted(table, key=ord):
        lines.append("%s\t%s" % (k, table[k]))
    return _emit(path, "\n".join(lines) + "\n", check)


def _emit(path, text, check):
    if check:
        old = io.open(path, encoding="utf-8").read() if os.path.exists(path) else None
        if old == text:
            print("  [同] %s" % path)
            return True
        print("  [異] %s —— 產生器算出來的內容與檔案不同" % path)
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    io.open(path, "w", encoding="utf-8").write(text)
    print("  [寫] %s（%d 位元組）" % (path, len(text.encode("utf-8"))))
    return True


# ────────────────────────────────────────────────────────────── 主流程 ───
def convert_chars(opencc_bin, cfg, chars):
    """一行一個字送進 opencc，拿回轉換後的結果。"""
    inp = ("\n".join(chars) + "\n").encode("utf-8")
    p = subprocess.run([opencc_bin, "-c", cfg], input=inp, capture_output=True)
    if p.returncode != 0:
        raise SystemExit("opencc 失敗：%s" % p.stderr.decode("utf-8", "replace"))
    out = p.stdout.decode("utf-8").split("\n")
    if len(out) < len(chars):
        raise SystemExit("opencc 回傳的行數比送進去的少")
    return dict(zip(chars, out))


def build_extra(bad, target, other, tables, manual):
    """對每一個「轉完仍在字集外」的字找一個在字集內的替身。"""
    TS, ST, JP, TWV, HKV = tables["TS"], tables["ST"], tables["JP"], tables["TWV"], tables["HKV"]
    TWVr, HKVr, STr = reverse(TWV), reverse(HKV), reverse(ST)
    manual_map = {k: v for k, v, _ in manual}
    out = collections.OrderedDict()
    for C in sorted(bad, key=ord):
        if C in manual_map:
            out[C] = manual_map[C]
            continue
        cands = []
        # 1. 日本新字体：只在 C 兩套字裡都不是正字時才敢用
        if C not in target and C not in other:
            for k in JP.get(C, []):
                cands.append(k)
                cands.extend(TS.get(k, []))
                cands.extend(ST.get(k, []))
        # 2. 臺／港字形的另一支
        for k in TWVr.get(C, []) + HKVr.get(C, []) + TWV.get(C, []) + HKV.get(C, []):
            cands.append(k)
            cands.extend(TS.get(k, []))
            cands.extend(ST.get(k, []))
        # 3. 直接的簡繁對應
        cands.extend(TS.get(C, []))
        cands.extend(ST.get(C, []))
        # 4. C 是某個簡化字的繁體形
        cands.extend(STr.get(C, []))
        for d in cands:
            if d in target and d != C:
                out[C] = d
                break
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=root)
    ap.add_argument("--opencc-src", default=os.path.join(
        root, "third_party/librime/deps/opencc/data/dictionary"))
    ap.add_argument("--opencc-bin", default=os.path.join(
        root, "third_party/build/host-opencc-install/bin/opencc"))
    ap.add_argument("--opencc-cfg", default=os.path.join(root, "core/data/shared/opencc"))
    ap.add_argument("--hans-source", default=None)
    ap.add_argument("--dict", default=os.path.join(root, "core/data/shared/luna_pinyin.dict.yaml"))
    ap.add_argument("--essay", default=os.path.join(root, "core/data/shared/essay.txt"))
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()

    lua_dir = os.path.join(a.repo, "core/data/lua")
    occ_dir = os.path.join(a.repo, "core/data/opencc")
    hans_mod = os.path.join(lua_dir, "luminakey_charset_hans.lua")
    hant_mod = os.path.join(lua_dir, "luminakey_charset_hant.lua")

    # ── 1. 兩個字集 ────────────────────────────────────────────────────
    if a.hans_source:
        HANS = hans_from_source(a.hans_source)
        origin_hans = ("《通用规范汉字表》(教育部／国家语委 2013)。"
                       "離線副本 %s，sha256 %s"
                       % (os.path.basename(a.hans_source), sha256(a.hans_source)[:16]))
    else:
        HANS = hans_from_module(hans_mod)
        origin_hans = None
    HANT = big5_han()
    print("簡體字集 %d 字、繁體字集 %d 字" % (len(HANS), len(HANT)))

    ok = True
    if origin_hans:
        ok &= write_lua_module(
            hans_mod, HANS, "簡體：《通用规范汉字表》", origin_hans,
            ["注意：這份表比表名的 8,105 多幾十個字，上游檔案本來就多收了，這裡照實收錄。",
             "『〇』不是漢字，不在這份表裡。"],
            a.check)
    ok &= write_lua_module(
        hant_mod, HANT, "繁體：Big5", "Python 的 big5 codec（等同 iconv 的 BIG5），無需下載",
        ["重算方式：對每一個漢字碼位試 ch.encode('big5')，成功的收進來。",
         "不用 big5hkscs：它收了 351 個真正的簡化字（国 学 发 个 东 业 众 …），",
         "當繁體判準等於把要擋的東西放進來。代價是 Big5 缺 酶 礴 珏 堃 喆 這類字。"],
        a.check)

    # ── 2. 兩張補充轉換表 ──────────────────────────────────────────────
    if not os.path.isdir(a.opencc_src) or not os.path.exists(a.opencc_bin):
        print("！找不到 opencc 原始詞典或 host 版執行檔，跳過補充轉換表。")
        print("  opencc-src=%s\n  opencc-bin=%s" % (a.opencc_src, a.opencc_bin))
        return 0 if ok else 1

    tables = {
        "TS": opencc_pairs(os.path.join(a.opencc_src, "TSCharacters.txt")),
        "ST": opencc_pairs(os.path.join(a.opencc_src, "STCharacters.txt")),
        "JP": opencc_pairs(os.path.join(a.opencc_src, "JPShinjitaiCharacters.txt")),
        "TWV": opencc_pairs(os.path.join(a.opencc_src, "TWVariants.txt")),
        "HKV": opencc_pairs(os.path.join(a.opencc_src, "HKVariants.txt")),
    }

    # 詞庫實際用到的漢字（加權只用來排報告，不影響產出）
    used = collections.Counter()
    freq = {}
    for line in io.open(a.essay, encoding="utf-8"):
        p = line.rstrip("\n").split("\t")
        if len(p) >= 2:
            try:
                freq[p[0]] = int(p[1])
            except ValueError:
                pass
    for r in rime_dict_rows(a.dict):
        f = freq.get(r[0], 1)
        for ch in r[0]:
            if is_han(ch):
                used[ch] += f
    chars = sorted(used, key=ord)
    print("詞庫用到 %d 個相異漢字" % len(chars))

    t2s = convert_chars(a.opencc_bin, os.path.join(a.opencc_cfg, "t2s.json"), chars)
    t2tw = convert_chars(a.opencc_bin, os.path.join(a.opencc_cfg, "t2tw.json"), chars)

    def residue(conv, target):
        w = collections.Counter()
        for c, n in used.items():
            for ch in conv[c]:
                if is_han(ch) and ch not in target:
                    w[ch] += n
        return w

    bad_s, bad_t = residue(t2s, HANS), residue(t2tw, HANT)
    total = sum(used.values())
    print("  t2s  轉完仍在字集外：%d 個相異字，加權 %.2f%%"
          % (len(bad_s), 100.0 * sum(bad_s.values()) / total))
    print("  t2tw 轉完仍在字集外：%d 個相異字，加權 %.2f%%"
          % (len(bad_t), 100.0 * sum(bad_t.values()) / total))

    ext_s = build_extra(bad_s, HANS, HANT, tables, MANUAL_T2S)
    ext_t = build_extra(bad_t, HANT, HANS, tables, MANUAL_T2TW)
    # 補表的**輸出**再過一次同一條鏈，免得吐出「在字集內但不是這個地區的字形」
    norm_s = convert_chars(a.opencc_bin, os.path.join(a.opencc_cfg, "t2s.json"),
                           sorted(set(ext_s.values())))
    norm_t = convert_chars(a.opencc_bin, os.path.join(a.opencc_cfg, "t2tw.json"),
                           sorted(set(ext_t.values())))
    for k in list(ext_s):
        v = norm_s.get(ext_s[k], ext_s[k])
        if len(v) == 1 and v in HANS:
            ext_s[k] = v
    for k in list(ext_t):
        v = norm_t.get(ext_t[k], ext_t[k])
        if len(v) == 1 and v in HANT:
            ext_t[k] = v

    # 自我檢查：key 一定在字集外、value 一定在字集內、不可以是恆等
    for name, tab, target in (("t2s", ext_s, HANS), ("t2tw", ext_t, HANT)):
        for k, v in tab.items():
            if k in target or v not in target or k == v:
                raise SystemExit("%s 補表有壞條目：%s→%s" % (name, k, v))

    def cover(bad, ext):
        tot = sum(bad.values())
        return 100.0 * sum(bad[c] for c in ext if c in bad) / max(tot, 1)

    print("  t2s  補表 %d 條，蓋掉表外加權的 %.1f%%" % (len(ext_s), cover(bad_s, ext_s)))
    print("  t2tw 補表 %d 條，蓋掉表外加權的 %.1f%%" % (len(ext_t), cover(bad_t, ext_t)))

    ok &= write_text_dict(
        os.path.join(occ_dir, "luminakey_t2s_extra.txt"), ext_s,
        "繁→簡：補 t2s 轉不掉的那些",
        ["對照的字集是《通用规范汉字表》。轉得掉的先轉掉——轉掉不損失候選，濾掉會。",
         "手工條目（opencc 一條都沒有的）：" + "、".join(
             "%s→%s（%s）" % (k, v, why) for k, v, why in MANUAL_T2S)],
        a.check)
    ok &= write_text_dict(
        os.path.join(occ_dir, "luminakey_t2tw_extra.txt"), ext_t,
        "繁→臺灣字形：補 t2tw 轉不掉的那些",
        ["對照的字集是 Big5。",
         "手工條目：" + "、".join(
             "%s→%s（%s）" % (k, v, why) for k, v, why in MANUAL_T2TW)],
        a.check)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
