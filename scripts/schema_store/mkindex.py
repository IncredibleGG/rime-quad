#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""方案市集 — 產生 index.json（規範 docs/schema-store.md §1）。

品質閘門：verified.deployed 不為 true 的套件**不進索引**。這裡不做例外。
"""
import datetime, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import languages   # noqa: E402  —— 語言標記，見 languages.py 檔頭

WORK = sys.argv[1]
DIST = sys.argv[2]
BASE_URL = sys.argv[3]

data = json.load(open(os.path.join(WORK, "packages.json")))
pkgs = data["packages"]
excluded = data["excluded"]
VDIR = os.path.join(WORK, "verify")

CATEGORIES = [
    {"id": "mandarin",  "name": "華語",     "order": 1},
    {"id": "topolect",  "name": "方言",     "order": 2},
    {"id": "other",     "name": "其他語系", "order": 3},
    {"id": "essential", "name": "基礎元件", "order": 9, "hidden": True},
]

# 顯示名稱與「這適合誰」。description 的目的是幫使用者**選**，不是描述技術細節，
# 所以一律寫人話，不抄上游 README。
META = {
 "luna-pinyin": ("朙月拼音",
   "最通用的全拼，不知道選什麼就選這個。詞庫是繁體，可切簡體輸出。", True),
 "terra-pinyin": ("地球拼音",
   "要按數字標聲調的全拼。想順便把聲調打對、或做語言學筆記的人適合；"
   "只想快點打字的話選朙月拼音。", False),
 "bopomofo": ("注音",
   "臺灣人最熟悉的注音，鍵位是大千（就是 Windows 內建注音那套）。"
   "從內建注音換過來不必重學。", True),
 "double-pinyin": ("雙拼",
   "每個字固定兩鍵，熟了比全拼少按一半。收錄了自然碼、小鶴、微軟、智慧 ABC 等"
   "幾套鍵位，要先背鍵位表，不適合第一次用。", False),
 "pinyin-simp": ("袖珍簡拼",
   "簡體全拼，詞庫小、反應快。只打簡體、又在意安裝大小的人適合。", False),
 "combo-pinyin": ("宮保拼音",
   "和弦式拼音 —— 同時按下幾個鍵拼成一個音節。要重學一套指法，"
   "換來的是很快的速度。給願意投資時間的人。", False),
 "stenotype": ("打字速記法",
   "一次按下一整個音節的速記式輸入，追求極速的實驗性方案。需要拼音詞庫。", False),
 "cangjie": ("倉頡五代",
   "拆字形輸入，不必知道怎麼唸就能打，打生僻字最強。代價是要先學拆碼規則。", False),
 "quick": ("速成",
   "倉頡的簡化版，只取頭尾兩碼，好學但重碼多、要常選字。香港很常見。", False),
 "wubi": ("五筆·86",
   "大陸最主流的字形輸入法，不看讀音、重碼少、打熟了很快。要先背字根。", False),
 "array": ("行列 30",
   "臺灣的字形輸入法，鍵位依筆形分區、不必背字根表。", False),
 "scj": ("快速倉頡",
   "倉頡的另一種簡化取碼，比速成精確一些。已經會倉頡的人上手最快。", False),
 "zhengma": ("鄭碼",
   "字形輸入法，字根依部件歸類，設計上比五筆規則性強。", False),
 "stroke": ("五筆畫",
   "只用「橫豎撇捺折」五個鍵，不必學任何編碼，打不出來的字用它救。"
   "同時也是其他方案的筆畫反查詞庫。", False),
 "c2h6-pinyin": ("乙烷拼音",
   "簡體取向的全拼變體，詞庫與朙月拼音不同。", False),
 "zrm": ("自然碼＋輔助碼",
   "自然碼雙拼再加一碼輔助碼來消重碼。給已經在用雙拼、嫌選字太多的人。", False),
 "radical-pinyin": ("部件拆字",
   "拼音打不出來的字，可以改用部件描述來反查。當作救援工具用。", False),
 "handarin": ("韓國式官話",
   "給以韓語為母語者設計的華語拼音方案。", False),
 "cantonese": ("粵語拼音（粵拼）",
   "目前維護最勤的粵語方案，詞庫大、口語詞齊全。想用粵語打字就選這個。", True),
 "jyutping": ("粵拼（傳統版）",
   "較早期的粵拼方案，詞庫較小但很輕。粵拼的另一個選擇。", False),
 "naamning-jyutping": ("南寧白話 / 平話",
   "廣西南寧一帶的粵語變體，附國際音標對照。", False),
 "wugniu": ("上海吳語",
   "上海話拼音方案，附老派（lopha）與新派兩種。", False),
 "soutzoe": ("蘇州吳語",
   "蘇州話拼音方案。", False),
 "dieghv": ("潮州話拼音",
   "閩南語潮汕片，收錄潮州、汕頭、揭陽、澄海等多個口音。", False),
 "middle-chinese": ("中古漢語拼音",
   "以《廣韻》音系打字，讀古籍、研究音韻學用。不是日常輸入法。", False),
 "sautungva": ("湘語·邵東話",
   "湖南邵東話拼音，另附西里爾字母轉寫。", False),
 "hangul": ("도한글（韓文）",
   "韓文輸入，打韓文字母組成音節。", True),
 "kappajp": ("河童五筆（日文）",
   "日文輸入，用羅馬字打假名與漢字。", True),
 "ipa": ("國際音標",
   "打 IPA 音標符號，兩套轉寫（X-SAMPA 與雲龍）。語言學筆記、標音用。", True),
 # ── 以下五套都需要 librime-lua（本專案已把它編進 librime，見 manifest.json）
 "ice": ("雾凇拼音",
   "目前最多人用的簡體拼音配置：詞庫大、開箱即用，還內建英文混輸、日期時間、"
   "計算器等功能。想要一套「別人已經調好」的拼音就選這個。", True),
 "moran": ("萬象拼音",
   "以雙拼為核心的大型整合方案，可加輔助碼消重碼，詞庫非常大（下載約 30MB）。"
   "適合已經熟悉雙拼、追求效率的人；第一次用拼音別從這裡開始。", False),
 "openfly": ("小鶴音形",
   "小鶴雙拼再配上字形輔助碼，重碼很少、打長句很順。要先學小鶴鍵位加字形碼。", False),
 "keydo": ("鍵道·我流",
   "音形碼頂功方案：編碼打滿就自動上屏，幾乎不用選字。代價是要重學一整套編碼規則。",
   False),
 "wubi86-jidian": ("五筆86·極點",
   "五筆86 的極點版配置，附拼音混輸與擴充詞庫。已經會五筆的人換過來最順。", False),
 "prelude": ("基礎配置",
   "default.yaml、標點、符號與按鍵綁定。所有方案都要它。", False),
 "essay": ("語料庫（繁）",
   "詞頻語言模型 essay.txt，決定整句輸入的候選排序。幾乎所有方案都要它。", False),
 "essay-simp": ("語料庫（簡）",
   "簡體版詞頻語言模型，簡體方案使用。", False),
}

verified = {}
for p in pkgs:
    fp = os.path.join(VDIR, p["id"] + ".json")
    verified[p["id"]] = json.load(open(fp)) if os.path.exists(fp) else None

entries, dropped = [], []
kept_ids = set()
for p in sorted(pkgs, key=lambda x: x["id"]):
    v = verified.get(p["id"])
    if not v or not v.get("deployed"):
        if not p["schemas"]:
            why = ("此套件不提供任何方案，也沒有任何通過驗證的套件相依它 —— "
                   "拿不到部署證據，且對使用者沒有用途")
        else:
            why = " / ".join((v or {}).get("notes", ["沒有驗證結果"]))[:400]
        dropped.append({"id": p["id"], "repo": p["repo"], "stage": "verify",
                        "reason": "librime 部署驗證未通過：" + why})
        continue
    if p["schemas"] and not v.get("schemas_ok"):
        dropped.append({"id": p["id"], "repo": p["repo"], "stage": "verify",
                        "reason": "沒有任何方案通過部署"})
        continue
    # ── 用到 lua 的套件：deployed 不算數，必須真的打出字。
    # 缺 librime-lua 時部署會回報 SUCCESS，只是引擎少了 translator/filter，
    # 按下去沒有任何候選 —— deploy-only 閘門抓不到這種假成功。既然這一批
    # 就是因為 lua 才進得來，閘門也就必須站在「輸入探針」這一關。
    if p.get("needs_lua") and not (v.get("probe") or {}).get("keys"):
        dropped.append({"id": p["id"], "repo": p["repo"], "stage": "verify",
                        "reason": "方案用到 lua 元件，部署雖然成功，但輸入探針打不出"
                                  "任何字 —— 這正是「缺 lua 會假成功」的樣子，不放行。"
                                  + (" lua 錯誤：" + "；".join(
                                      (v.get("lua") or {}).get("errors") or [])[:300])})
        continue
    kept_ids.add(p["id"])

# 相依必須也在索引裡，否則行動端展開 requires 會落空
changed = True
while changed:
    changed = False
    for pid in sorted(kept_ids):
        p = next(x for x in pkgs if x["id"] == pid)
        miss = [q for q in p["requires"] if q not in kept_ids]
        if miss:
            kept_ids.discard(pid)
            dropped.append({"id": pid, "repo": p["repo"], "stage": "index",
                            "reason": f"相依的套件未通過驗證，無法成套提供：{miss}"})
            changed = True

# ── 語言標記（BCP 47）─────────────────────────────────────────────────────
# 判定規則與依據見 languages.py。這裡只把結果掛上去。
# 為什麼 key 是 (套件, 方案) 而不是方案：**方案 id 不是全域唯一的** ——
# double_pinyin 同時存在於 double-pinyin（繁體詞庫）與 ice（簡體詞庫）。
_schemas_ok = {p["id"]: set((verified.get(p["id"]) or {}).get("schemas_ok") or [])
               for p in pkgs}
_lr, _rows, _builtin = languages.build(WORK, only_pkgs=kept_ids, schemas_of=_schemas_ok)
lang_of = {(r["package"], r["schema"]): r["language"] for r in _rows}
lang_src = {(r["package"], r["schema"]): r["source"] for r in _rows}

for p in sorted(pkgs, key=lambda x: x["id"]):
    if p["id"] not in kept_ids:
        continue
    v = verified[p["id"]]
    ok = set(v.get("schemas_ok") or [])
    name, desc, rec = META.get(
        p["id"], (p.get("rppi_name") or p["id"], "（尚無中文說明）", False))
    e = {
        "id": p["id"],
        "name": name,
        "category": p["category"],
        "description": desc,
        "upstream": p["upstream"],
        "upstream_commit": p["commit"][:7],
        "license": p["license"],
        "file": p["zip"],
        "size": p["size"],
        "sha256": p["sha256"],
        "schemas": [
            {"id": s["id"], "name": s["name"],
             "language": lang_of.get((p["id"], s["id"]), "und")}
            for s in p["schemas"] if s["id"] in ok
        ],
        "requires": p["requires"],
        "recommended_layout": p["recommended_layout"],
        "verified": {"deployed": True},
    }
    if p.get("layout_note"):
        e["layout_note"] = p["layout_note"]
    if rec:
        e["recommended"] = True
    if v.get("probe"):
        e["verified"]["probe"] = v["probe"]
    entries.append(e)

idx = {
    "format_version": 1,
    "generated_at": datetime.datetime.now(datetime.timezone.utc)
                    .replace(microsecond=0).isoformat().replace("+00:00", "Z"),
    "base_url": BASE_URL,
    "categories": CATEGORIES,
    # 語言分組表。顯示名放在索引裡而不是 app 裡，理由與 categories 相同：
    # 新增一種語言不該要求使用者先更新 app。
    "languages": languages.LANGUAGES,
    # 隨 APK 出貨的內建方案不在 packages 裡（它們不是從市集裝的），
    # 但選單一樣要分組，所以在這裡補一份對照。
    "builtin_schemas": [{"id": b["id"], "name": b["name"], "language": b["language"]}
                        for b in _builtin],
    "packages": entries,
}
out = os.path.join(DIST, "index.json")
json.dump(idx, open(out, "w"), ensure_ascii=False, indent=1)

data["excluded"] = excluded + dropped
data["index_ids"] = sorted(kept_ids)
json.dump(data, open(os.path.join(WORK, "packages.json"), "w"),
          ensure_ascii=False, indent=1)

# 只留還在索引裡的 zip，避免上傳孤兒檔
alive = {e["file"] for e in entries} | {"index.json"}
for fn in os.listdir(DIST):
    fp = os.path.join(DIST, fn)
    if fn not in alive and os.path.isfile(fp) and fn.endswith((".zip", ".json")):
        os.remove(fp)

print(f"index.json: {len(entries)} 個套件；排除 {len(dropped)} 個")
by_cat = {}
for e in entries:
    by_cat.setdefault(e["category"], []).append(e["id"])
for c in CATEGORIES:
    print(f"  {c['id']:10s} {len(by_cat.get(c['id'], []))}: "
          f"{' '.join(by_cat.get(c['id'], []))}")
_tagged = [e for e in entries for s2 in e["schemas"]]
_all = [(e["id"], s2) for e in entries for s2 in e["schemas"]]
_und = [(pid, s2["id"]) for pid, s2 in _all if s2["language"] == "und"]
print("\n語言標記：%d 個方案，und %d 個（%.1f%%）"
      % (len(_all), len(_und), 100.0 * len(_und) / max(len(_all), 1)))
for pid, sid in _und:
    print("  und  %-24s %-16s %s" % (sid, pid, lang_src.get((pid, sid), "")))
print("\n排除清單：")
for d in dropped:
    print(f"  {d['id']:22s} [{d['stage']}] {d['reason'][:150]}")
