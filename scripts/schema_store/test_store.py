#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_store.py — 方案市集伺服器側的測試。

跑法
    python3 scripts/schema_store/test_store.py            # 用版控裡的真實語料
    python3 scripts/schema_store/test_store.py --require-live
        # 額外要求現場資料（build/schema-store/_work）存在；沒有就紅。
        # 跑過完整流水線之後（CI／本機 build）用這個。

這份測試的三條紀律
─────────────────────────────────────────────────────────────────────────────
1. **拿真的 34 個套件跑。** 語料 `data/corpus.json` 是從真實的
   `_work/packages.json` 脫水出來的（見 snapshot.py），不是編的。用假資料只會
   證明「偵測器認得我自己編的例子」。

2. **不准安靜地跳過自己。** 需要現場資料的測試在資料不在時會 skip，但收尾時
   一定會印出一塊醒目的「以下 N 項沒有跑」，而且 `--require-live` 會讓它變成
   失敗。這個專案有過「發布關卡的升級測試因步驟順序寫反被判略過，報出一片
   全綠」，同一種事不要再發生一次。

3. **每一個偵測器都要有反向測試。** 光證明「偵測器在現況下說 OK」沒有用 ——
   要證明它在該紅的時候會紅。所以下面每個 golden 比對都配一組**植入違規**
   的測試：故意造一個撞號、故意拿掉一個撞號、故意在資料檔裡寫一條危險的
   裸 id，並斷言比對函式真的抓得到。
"""

from __future__ import annotations

import copy
import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import languages   # noqa: E402
import registry    # noqa: E402
import uid as uidlib   # noqa: E402

DATA_DIR = os.path.join(HERE, "data")
CORPUS = os.path.join(DATA_DIR, "corpus.json")
KNOWN = os.path.join(DATA_DIR, "known_collisions.yaml")

# 現場資料（跑過流水線才有）。沒有時只有少數幾條測試會 skip。
LIVE_WORK = os.environ.get("RIME_STORE_WORK") or os.path.abspath(
    os.path.join(HERE, "..", "..", "build", "schema-store", "_work"))
LIVE_AVAILABLE = os.path.isfile(os.path.join(LIVE_WORK, "packages.json"))

# 版控裡的語料是從這麼多個套件抽出來的。數字寫死是刻意的：
# 有人把語料換成三個假套件時，測試要紅。
EXPECTED_PACKAGE_COUNT = 34


def load_corpus():
    with open(CORPUS, encoding="utf-8") as f:
        return json.load(f)


def load_known():
    import yaml
    with open(KNOWN, encoding="utf-8") as f:
        return yaml.safe_load(f)


# ── 比對器：偵測到的 vs 黃金清單 ────────────────────────────────────────────
# 抽成函式是為了讓**反向測試**打得到它 —— 直接在測試裡 assertEqual 的話，
# 沒有辦法證明「多一個」或「少一個」真的會被抓出來。

def diff_schema_collisions(detected, known_doc):
    """→ (只在現場有的, 只在黃金清單有的, 提供者對不上的)"""
    known = {c["schema"]: sorted(c["providers"]) for c in known_doc["schema_ids"]}
    extra = sorted(set(detected) - set(known))
    missing = sorted(set(known) - set(detected))
    mismatch = sorted(sid for sid in set(detected) & set(known)
                      if sorted(detected[sid]) != known[sid])
    return extra, missing, mismatch


def diff_file_conflicts(detected, known_doc):
    known = {c["entry"]: sorted(c["packages"]) for c in known_doc["files"]}
    extra = sorted(set(detected) - set(known))
    missing = sorted(set(known) - set(detected))
    mismatch = sorted(e for e in set(detected) & set(known)
                      if sorted(detected[e]) != known[e])
    return extra, missing, mismatch


# ── 舊讀取端模擬器 ──────────────────────────────────────────────────────────

class OldReaderRejected(Exception):
    pass


def old_reader_parse(index):
    """模擬**已經出貨**的 Android IndexParser（v1）看到一份索引會得到什麼。

    照抄現行行為，包括它的毛病：
      · `format_version != 1` → **整份拒收**（不是「大於才拒收」）。
      · 不認得的鍵一律忽略。
      · 沒有 sha256 / sha256 格式不對 → 那個套件出局。
    這支的用途是回答「舊 app 讀到新索引會怎樣」，所以它必須長得像舊 app，
    不是長得像我們希望的樣子。
    """
    if index.get("format_version") != 1:
        raise OldReaderRejected(
            "format_version=%r，舊 app 只認 1" % index.get("format_version"))
    out = []
    for p in index.get("packages") or []:
        sha = (p.get("sha256") or "").lower()
        if len(sha) != 64 or any(c not in "0123456789abcdef" for c in sha):
            continue
        if not p.get("file"):
            continue
        out.append({
            "id": p.get("id"),
            "name": p.get("name"),
            "category": p.get("category"),
            "file": p.get("file"),
            "sha256": sha,
            "schemas": [{"id": s.get("id"), "name": s.get("name"),
                         "language": s.get("language")}
                        for s in (p.get("schemas") or [])],
            "requires": p.get("requires") or [],
            "deployed": bool((p.get("verified") or {}).get("deployed")),
            "recommended_layout": p.get("recommended_layout"),
        })
    return out


def strip_to_v1(index):
    """把新欄位全部拿掉，得到「這份索引在加欄位之前長什麼樣」。"""
    NEW_TOP = {"format_minor", "schema_id_collisions", "language_coverage"}
    NEW_PKG = {"conflicts"}
    NEW_SCHEMA = {"uid", "language_source"}
    out = {k: v for k, v in index.items() if k not in NEW_TOP}
    out["packages"] = []
    for p in index.get("packages") or []:
        q = {k: v for k, v in p.items() if k not in NEW_PKG}
        q["schemas"] = [{k: v for k, v in s.items() if k not in NEW_SCHEMA}
                        for s in (p.get("schemas") or [])]
        out["packages"].append(q)
    out["builtin_schemas"] = [{k: v for k, v in b.items() if k not in NEW_SCHEMA}
                              for b in (index.get("builtin_schemas") or [])]
    return out


# ═══════════════════════════════════════════════════════════ 語料本身 ══════

class TestCorpus(unittest.TestCase):

    def test_corpus_is_the_real_34_packages(self):
        c = load_corpus()
        self.assertEqual(c["package_count"], len(c["packages"]))
        self.assertEqual(
            c["package_count"], EXPECTED_PACKAGE_COUNT,
            "版控裡的語料應該是真實的 %d 個套件；現在是 %d 個。"
            "若上游真的變了，跑 snapshot.py 重新產生並更新 EXPECTED_PACKAGE_COUNT。"
            % (EXPECTED_PACKAGE_COUNT, c["package_count"]))
        self.assertTrue(all(p.get("entries") for p in c["packages"]),
                        "有套件沒有任何 zip entry —— 語料不完整，撞名偵測會失真")

    def test_corpus_has_language_results(self):
        c = load_corpus()
        self.assertIn("languages", c)
        nsch = sum(len(p["schemas"]) for p in c["packages"])
        self.assertEqual(len(c["languages"]), nsch,
                         "語料裡的語言判定筆數與方案數對不上")


# ═════════════════════════════════════════════════════════════ uid ════════

class TestUid(unittest.TestCase):

    def setUp(self):
        self.c = load_corpus()

    def test_uid_is_unique_over_the_real_corpus(self):
        seen = {}
        for p in self.c["packages"]:
            for s in p["schemas"]:
                u = uidlib.make_uid(p["id"], s["id"])
                self.assertNotIn(u, seen, "uid 撞了：%s" % u)
                seen[u] = True
        for b in self.c.get("builtin") or []:
            u = uidlib.builtin_uid(b["id"])
            self.assertNotIn(u, seen, "uid 撞了（內建）：%s" % u)
            seen[u] = True
        self.assertGreater(len(seen), 90)

    def test_bare_schema_id_is_NOT_unique(self):
        """這條是在釘住「問題是真的」。

        如果哪天它變綠了（撞號消失），下面的黃金比對也會紅，那時再一起處理。
        重點是：**不要讓「裸 id 夠用」這個假設無聲地復活**。
        """
        cols = uidlib.schema_collisions(self.c["packages"], self.c.get("builtin"))
        self.assertTrue(cols, "真實語料裡竟然沒有任何 schema id 撞號？"
                              "先確認語料是不是被換成假資料了")

    def test_roundtrip_over_every_real_pair(self):
        for p in self.c["packages"]:
            for s in p["schemas"]:
                u = uidlib.make_uid(p["id"], s["id"])
                owner, sid = uidlib.parse_uid(u)
                self.assertEqual(owner, p["id"])
                self.assertEqual(sid, s["id"])

    def test_local_uid_roundtrip_and_sanitising(self):
        u = uidlib.local_uid("我的/方案 v2.zip", "my_schema")
        owner, sid = uidlib.parse_uid(u)
        self.assertEqual(sid, "my_schema")
        self.assertTrue(owner.startswith(uidlib.LOCAL_NS + "/"))
        self.assertNotIn(" ", u)
        # 同一個來源兩次匯入 → 同一個 uid（那本來就是「重裝」）
        self.assertEqual(u, uidlib.local_uid("我的/方案 v2.zip", "my_schema"))

    def test_reserved_namespaces_cannot_be_a_package_id(self):
        for bad in (uidlib.BUILTIN_NS, uidlib.LOCAL_NS, "@x"):
            with self.assertRaises(uidlib.UidError):
                uidlib.check_package_id(bad)

    def test_rejects_malformed_ids(self):
        for bad in ("Has-Upper", "-leading", "with/slash", "", None, "with space"):
            with self.assertRaises(uidlib.UidError):
                uidlib.check_package_id(bad)
        for bad in ("with/slash", "", None, "with space", "-leading"):
            with self.assertRaises(uidlib.UidError):
                uidlib.check_schema_id(bad)

    def test_parse_uid_rejects_garbage(self):
        for bad in ("noslash", "/", "abc/", "/abc", None, 3):
            with self.assertRaises(uidlib.UidError):
                uidlib.parse_uid(bad)

    def test_real_ids_all_pass_the_shape_rules(self):
        """真實資料裡的 id 必須真的符合我們宣稱的形狀，否則 uid 規則是空話。"""
        for p in self.c["packages"]:
            uidlib.check_package_id(p["id"])
            for s in p["schemas"]:
                uidlib.check_schema_id(s["id"])


# ══════════════════════════════════════════════ 撞號偵測（含反向測試）══════

class TestCollisions(unittest.TestCase):

    def setUp(self):
        self.c = load_corpus()
        self.known = load_known()

    # ── 正向：現場 == 黃金清單 ──────────────────────────────────────────
    def test_schema_collisions_match_the_golden_list(self):
        det = uidlib.schema_collisions(self.c["packages"], self.c.get("builtin"))
        extra, missing, mismatch = diff_schema_collisions(det, self.known)
        self.assertEqual(
            (extra, missing, mismatch), ([], [], []),
            "撞號清單與真實語料對不上。\n"
            "  多出來（上游新增了撞號，要有人決定怎麼辦）：%s\n"
            "  少掉（上游改名了，該從 known_collisions.yaml 刪掉）：%s\n"
            "  提供者對不上：%s" % (extra, missing, mismatch))

    def test_file_conflicts_match_the_golden_list(self):
        det = uidlib.file_conflicts(self.c["packages"])
        extra, missing, mismatch = diff_file_conflicts(det, self.known)
        self.assertEqual((extra, missing, mismatch), ([], [], []),
                         "檔案撞名清單對不上。多出：%s；少掉：%s；擁有者不符：%s"
                         % (extra, missing, mismatch))

    def test_the_six_dangerous_ones_are_all_there(self):
        """`double_pinyin` 那一組必須在清單裡，而且標成 content-differs。"""
        by_id = {c["schema"]: c for c in self.known["schema_ids"]}
        for sid in ("double_pinyin", "double_pinyin_abc", "double_pinyin_flypy",
                    "double_pinyin_mspy", "pinyin_simp", "radical_pinyin"):
            self.assertIn(sid, by_id)
            self.assertEqual(by_id[sid]["severity"], "content-differs", sid)

    # ── 反向：植入違規，斷言比對函式會紅 ────────────────────────────────
    def test_MUTATION_injected_collision_is_caught(self):
        c = copy.deepcopy(self.c)
        # 讓 cangjie 也提供一個叫 luna_pinyin 的方案 —— 現實中沒有，
        # 但如果哪天真的出現，偵測器必須看得到。
        target = next(p for p in c["packages"] if p["id"] == "cangjie")
        target["schemas"].append({"id": "luna_pinyin", "name": "假的"})
        det = uidlib.schema_collisions(c["packages"], c.get("builtin"))
        self.assertIn("cangjie", det.get("luna_pinyin", []))
        extra, missing, mismatch = diff_schema_collisions(det, self.known)
        self.assertTrue(extra or mismatch,
                        "植入了一個新撞號，比對函式竟然沒有反應")

    def test_MUTATION_removed_collision_is_caught(self):
        c = copy.deepcopy(self.c)
        ice = next(p for p in c["packages"] if p["id"] == "ice")
        ice["schemas"] = [s for s in ice["schemas"] if s["id"] != "double_pinyin"]
        det = uidlib.schema_collisions(c["packages"], c.get("builtin"))
        self.assertNotIn("double_pinyin", det)
        extra, missing, mismatch = diff_schema_collisions(det, self.known)
        self.assertIn("double_pinyin", missing,
                      "撞號消失了，比對函式應該要說黃金清單裡多了一條")

    def test_MUTATION_injected_file_conflict_is_caught(self):
        c = copy.deepcopy(self.c)
        target = next(p for p in c["packages"] if p["id"] == "quick")
        target["entries"] = sorted(set(target["entries"]) | {"punctuation.yaml"})
        det = uidlib.file_conflicts(c["packages"])
        self.assertIn("quick", det["punctuation.yaml"])
        extra, missing, mismatch = diff_file_conflicts(det, self.known)
        self.assertTrue(mismatch or extra,
                        "植入了一個檔案撞名，比對函式竟然沒有反應")

    def test_conflicts_by_package_is_symmetric_and_non_empty(self):
        cbp = uidlib.conflicts_by_package(self.c["packages"])
        self.assertIn("ice", cbp)
        for a, peers in cbp.items():
            for peer in peers:
                b = peer["package"]
                self.assertIn(b, cbp, "%s 說會撞 %s，反過來卻沒有" % (a, b))
                back = [x for x in cbp[b] if x["package"] == a]
                self.assertTrue(back, "%s ↔ %s 不對稱" % (a, b))
                self.assertEqual(sorted(peer["files"]), sorted(back[0]["files"]))


# ═════════════════════════════════════════════════════ 語言標記 ═══════════

class TestLanguages(unittest.TestCase):

    def setUp(self):
        self.c = load_corpus()
        self.data = languages.load_data()
        self.known = load_known()

    def test_data_file_validates(self):
        self.assertEqual(self.data.validate(), [])

    def test_MUTATION_broken_data_file_is_rejected(self):
        bad = copy.deepcopy(self.data)
        bad.schemas = dict(bad.schemas)
        bad.schemas["fake_one"] = {"tag": "zz-Zzzz", "why": "亂寫的"}
        self.assertTrue(bad.validate(), "資料檔驗證竟然放過了不存在的語言標記")
        bad2 = copy.deepcopy(self.data)
        bad2.schemas = dict(bad2.schemas)
        bad2.schemas["fake_two"] = {"tag": "zh-Hant"}      # 沒寫理由
        self.assertTrue(any("why" in e for e in bad2.validate()),
                        "資料檔驗證竟然放過了沒有理由的人工判定")

    def test_curated_bare_keys_never_cover_a_dangerous_collision(self):
        """裸 id 當鍵，等於對兩個內容相反的方案下同一個判斷。

        `double_pinyin` 那六個是 content-differs，人工判定必須寫成 uid。
        """
        dangerous = {c["schema"] for c in self.known["schema_ids"]
                     if c["severity"] == "content-differs"}
        offenders = [k for k in self.data.schemas
                     if "/" not in k and k in dangerous]
        self.assertEqual(offenders, [],
                         "languages.yaml 用裸 id 標了會撞號且內容相反的方案：%s"
                         "（請改成 <套件>/<方案>）" % offenders)

    def test_MUTATION_dangerous_bare_key_is_caught(self):
        dangerous = {c["schema"] for c in self.known["schema_ids"]
                     if c["severity"] == "content-differs"}
        fake = dict(self.data.schemas)
        fake["double_pinyin"] = {"tag": "zh-Hant", "why": "偷懶"}
        offenders = [k for k in fake if "/" not in k and k in dangerous]
        self.assertEqual(offenders, ["double_pinyin"],
                         "植入了一條危險的裸 id 判定，檢查竟然沒有抓到")

    def test_the_two_double_pinyin_really_get_opposite_scripts(self):
        """整條支線的起因，用真實判定結果釘住。"""
        lang = self.c["languages"]
        self.assertEqual(lang["double-pinyin/double_pinyin"]["language"], "zh-Hant")
        self.assertEqual(lang["ice/double_pinyin"]["language"], "zh-Hans")

    def test_every_row_has_a_traceable_source(self):
        ok = {"upstream", "curated", "derived", "unknown"}
        for u, r in self.c["languages"].items():
            self.assertIn(r["source"], ok, u)
            self.assertTrue(r["rule"], "%s 沒有 rule，來源追不回去" % u)

    def test_und_is_either_declared_not_a_language_or_unknown(self):
        """『沒把握就標未知』的另一半：und 不可以是「偷偷放棄」。"""
        for u, r in self.c["languages"].items():
            if r["language"] == "und":
                self.assertTrue(
                    r["not_a_language"] or r["source"] == "unknown",
                    "%s 標了 und，但既沒宣告 not_a_language 也不是 unknown" % u)
            else:
                self.assertNotEqual(r["source"], "unknown", u)

    def test_all_tags_are_in_the_language_table(self):
        for u, r in self.c["languages"].items():
            self.assertIn(r["language"], self.data.known_tags, u)

    def test_coverage_is_computable_and_reported(self):
        cov = self.c["coverage"]
        for k in ("total", "tagged", "not_a_language", "unknown", "coverage_pct"):
            self.assertIn(k, cov)
        self.assertEqual(cov["total"],
                         cov["tagged"] + cov["not_a_language"] + cov["unknown"])
        # 「還有幾個是未知」必須是一個講得出來的數字，不是「大概吧」。
        self.assertIsInstance(cov["unknown"], int)

    def _rows(self):
        return [{"uid": u, "schema": u.split("/", 1)[1], "package": u.split("/", 1)[0],
                 "language": r["language"]}
                for u, r in self.c["languages"].items()]

    def test_flatten_drops_only_the_ids_whose_verdicts_differ(self):
        """裸 id 扁平表的收錄規則：判定不同才丟，判定相同照收。

        `double_pinyin`（繁 vs 簡）必須丟；`pinyin_simp`（兩邊都 zh-Hans）
        必須留 —— 為了「內容不同」丟掉一個確定的正確答案，只會讓讀取端退回
        字面啟發式，那才是真的會分錯。
        """
        flat, by_uid, conflicts = languages.flatten(self._rows(), [])
        self.assertIn("double_pinyin", conflicts)
        self.assertNotIn("double_pinyin", flat)
        self.assertNotIn("pinyin_simp", conflicts)
        self.assertEqual(flat.get("pinyin_simp"), "zh-Hans")
        self.assertEqual(flat.get("radical_pinyin"), "zh-Hans")

    def test_uid_table_never_has_to_drop_anything(self):
        """uid 表不必丟掉任何東西 —— 那正是 uid 的用處。"""
        rows = self._rows()
        _, by_uid, _ = languages.flatten(rows, [])
        self.assertEqual(len(by_uid),
                         len([r for r in rows if r["language"] != "und"]))

    def test_MUTATION_flatten_drops_an_id_once_the_verdicts_diverge(self):
        rows = self._rows()
        for r in rows:
            if r["uid"] == "wubi86-jidian/pinyin_simp":
                r["language"] = "zh-Hant"      # 假裝其中一邊換了字集
        flat, _, conflicts = languages.flatten(rows, [])
        self.assertIn("pinyin_simp", conflicts)
        self.assertNotIn("pinyin_simp", flat,
                         "判定分歧之後，這個 id 應該自動從裸 id 表消失")


# ══════════════════════════════════════════════ 索引格式相容性 ════════════

SAMPLE_NEW = {
    "format_version": 1,
    "format_minor": 1,
    "generated_at": "2026-08-08T00:00:00Z",
    "base_url": "https://example.invalid/rime/schemas/",
    "categories": [{"id": "mandarin", "name": "華語", "order": 1}],
    "languages": [{"tag": "zh-Hant", "name": "中文（繁體）", "order": 3}],
    "builtin_schemas": [{"id": "luna_pinyin", "uid": "@builtin/luna_pinyin",
                         "name": "朙月拼音", "language": "zh-Hant",
                         "language_source": "derived"}],
    "schema_id_collisions": [{"schema": "double_pinyin",
                              "providers": ["double-pinyin", "ice"]}],
    "language_coverage": {"total": 2, "tagged": 2, "not_a_language": 0,
                          "unknown": 0, "coverage_pct": 100.0,
                          "by_source": {"derived": 2}},
    "packages": [{
        "id": "double-pinyin", "name": "雙拼", "category": "mandarin",
        "description": "…", "upstream": "https://example.invalid/x",
        "upstream_commit": "0123456", "license": "GPL-3.0-only",
        "file": "double-pinyin-0123456.zip", "size": 22453,
        "sha256": "a" * 64,
        "schemas": [{"id": "double_pinyin", "uid": "double-pinyin/double_pinyin",
                     "name": "自然碼雙拼", "language": "zh-Hant",
                     "language_source": "derived"}],
        "requires": ["prelude"], "recommended_layout": "qwerty",
        "verified": {"deployed": True},
        "conflicts": [{"package": "ice",
                       "files": ["double_pinyin.schema.yaml"],
                       "schemas": ["double_pinyin"]}],
    }],
}


class TestIndexCompat(unittest.TestCase):

    def test_old_reader_sees_exactly_the_same_thing_with_or_without_new_fields(self):
        """『舊 app 讀到新索引會怎樣』—— 答案必須是「跟以前一模一樣」。"""
        new = old_reader_parse(SAMPLE_NEW)
        old = old_reader_parse(strip_to_v1(SAMPLE_NEW))
        self.assertEqual(new, old)
        self.assertEqual(len(new), 1)
        self.assertEqual(new[0]["schemas"][0]["language"], "zh-Hant")

    def test_MUTATION_bumping_major_breaks_every_shipped_app(self):
        """把 format_version 改成 2 會發生什麼事，用測試寫下來。

        不是「新 app 會升級」，是**所有已出貨的 app 立刻整份拒收索引**，
        市集變成空的。所以加欄位一律走 format_minor。
        """
        bumped = dict(SAMPLE_NEW, format_version=2)
        with self.assertRaises(OldReaderRejected):
            old_reader_parse(bumped)

    def test_minor_bump_is_invisible_to_old_readers(self):
        self.assertEqual(old_reader_parse(dict(SAMPLE_NEW, format_minor=99)),
                         old_reader_parse(SAMPLE_NEW))

    def test_uid_in_index_matches_package_and_schema_id(self):
        for p in SAMPLE_NEW["packages"]:
            for s in p["schemas"]:
                self.assertEqual(s["uid"], uidlib.make_uid(p["id"], s["id"]))


# ══════════════════════════════════════════ 安裝紀錄的遷移 ════════════════

REG_V1 = {
    "format_version": 1,
    "packages": [
        {"id": "double-pinyin", "name": "雙拼", "sha256": "b" * 64,
         "installed_at": 1, "source": "store", "recommended_layout": "qwerty",
         "layout_note": None, "requires": ["prelude"],
         "files": ["double_pinyin.schema.yaml"],
         "schemas": [{"id": "double_pinyin", "name": "自然碼雙拼",
                      "language": "zh-Hant"}]},
        {"id": "ice", "name": "雾凇拼音", "sha256": "c" * 64,
         "installed_at": 2, "source": "store", "recommended_layout": "qwerty",
         "layout_note": None, "requires": [],
         "files": ["double_pinyin.schema.yaml", "rime_ice.schema.yaml"],
         "schemas": [{"id": "double_pinyin", "name": "自然码双拼",
                      "language": "zh-Hans"},
                     {"id": "rime_ice", "name": "雾凇拼音",
                      "language": "zh-Hans"}]},
        {"id": "local:我的方案 v2", "name": "我的方案", "sha256": "",
         "installed_at": 3, "source": "local", "recommended_layout": None,
         "layout_note": None, "requires": [], "files": ["mine.schema.yaml"],
         "schemas": [{"id": "mine", "name": "mine", "language": None}]},
    ],
}


class TestRegistryMigration(unittest.TestCase):

    def test_every_schema_gets_a_uid(self):
        new, rep = registry.migrate(REG_V1)
        self.assertEqual(new["format_version"], 2)
        self.assertEqual(rep["added_uid"], 4)
        uids = [s["uid"] for p in new["packages"] for s in p["schemas"]]
        self.assertEqual(len(uids), len(set(uids)), "遷移後仍有 uid 相撞")
        self.assertIn("double-pinyin/double_pinyin", uids)
        self.assertIn("ice/double_pinyin", uids)

    def test_migration_is_lossless(self):
        """v1 已經以套件為單位存了，所以遷移不需要問任何人。"""
        new, _ = registry.migrate(REG_V1)
        for old_p, new_p in zip(REG_V1["packages"], new["packages"]):
            for k, v in old_p.items():
                if k == "schemas":
                    continue
                self.assertEqual(new_p[k], v, k)
            for old_s, new_s in zip(old_p["schemas"], new_p["schemas"]):
                for k, v in old_s.items():
                    self.assertEqual(new_s[k], v, k)

    def test_migration_is_idempotent(self):
        once, _ = registry.migrate(REG_V1)
        twice, rep2 = registry.migrate(once)
        self.assertEqual(once, twice)
        self.assertEqual(rep2["added_uid"], 0)

    def test_local_packages_use_the_reserved_namespace(self):
        new, _ = registry.migrate(REG_V1)
        local = [p for p in new["packages"] if p["source"] == "local"][0]
        u = local["schemas"][0]["uid"]
        self.assertTrue(uidlib.is_local_uid(u), u)
        self.assertEqual(uidlib.parse_uid(u)[1], "mine")

    def test_it_reports_the_files_it_cannot_repair(self):
        """兩個套件都宣稱擁有 double_pinyin.schema.yaml，磁碟上只有一份。

        遷移修不好，但**必須講出來**。
        """
        _, rep = registry.migrate(REG_V1)
        files = {c["file"]: c["packages"] for c in rep["file_conflicts"]}
        self.assertIn("double_pinyin.schema.yaml", files)
        self.assertEqual(files["double_pinyin.schema.yaml"],
                         ["double-pinyin", "ice"])

    def test_bad_records_are_kept_not_dropped(self):
        broken = copy.deepcopy(REG_V1)
        broken["packages"][0]["schemas"].append({"id": "bad/id", "name": "x"})
        new, rep = registry.migrate(broken)
        self.assertEqual(len(rep["invalid"]), 1)
        self.assertEqual(len(new["packages"][0]["schemas"]), 2,
                         "壞紀錄被丟掉了 —— 那會讓磁碟上的檔案變成孤兒")


# ═════════════════════════════════════ 需要現場資料的（會 skip，但會吵）══

class TestAgainstLiveData(unittest.TestCase):

    @unittest.skipUnless(LIVE_AVAILABLE, "沒有現場資料 %s" % LIVE_WORK)
    def test_corpus_matches_live(self):
        """版控裡的語料 == 現場重新抽一次的結果。

        沒有這條，`data/corpus.json` 會慢慢腐爛成「以前某個人跑過一次」。
        """
        import snapshot
        live = snapshot.build_corpus(LIVE_WORK, with_languages=True)
        saved = load_corpus()
        self.assertEqual(
            [p["id"] for p in live["packages"]],
            [p["id"] for p in saved["packages"]],
            "套件清單變了 —— 跑 snapshot.py 重新產生語料")
        for lp, sp in zip(live["packages"], saved["packages"]):
            self.assertEqual(lp["schemas"], sp["schemas"], lp["id"])
            self.assertEqual(lp["entries"], sp["entries"], lp["id"])
        self.assertEqual(live["languages"], saved["languages"],
                         "語言判定與現場不符 —— 跑 snapshot.py 重新產生語料")

    @unittest.skipUnless(LIVE_AVAILABLE, "沒有現場資料 %s" % LIVE_WORK)
    def test_live_language_resolution_has_no_hidden_guesses(self):
        data = languages.load_data()
        _, rows, builtin = languages.build(LIVE_WORK, data=data)
        for r in list(rows) + list(builtin):
            if r["language"] == "und":
                self.assertTrue(r.get("not_a_language") or r["source"] == "unknown",
                                r["uid"])
            else:
                self.assertIn(r["source"], ("upstream", "curated", "derived"))
                self.assertTrue(r["why"])


# ════════════════════════════════════════════════════════════ runner ══════

def main():
    require_live = "--require-live" in sys.argv
    argv = [a for a in sys.argv if a != "--require-live"]
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromModule(sys.modules[__name__])
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)

    # ── 不准安靜地跳過自己 ────────────────────────────────────────────
    if result.skipped:
        print("\n" + "!" * 70)
        print("!! 以下 %d 項測試**沒有跑**（不是通過）：" % len(result.skipped))
        for case, why in result.skipped:
            print("!!   %s\n!!     ↳ %s" % (case, why))
        print("!! 需要現場資料時先跑：scripts/build_schema_store.sh --phase index")
        print("!! 或指定 RIME_STORE_WORK=<_work 目錄>")
        print("!" * 70)
        if require_live:
            print("\n--require-live：有測試被略過，判定為失敗。", file=sys.stderr)
            return 1
    else:
        print("\n（沒有任何測試被略過）")

    print("\n共 %d 項，失敗 %d、錯誤 %d、略過 %d"
          % (result.testsRun, len(result.failures), len(result.errors),
             len(result.skipped)))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
