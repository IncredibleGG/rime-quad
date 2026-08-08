#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uid.py — 方案的**全域唯一識別碼**，以及撞號偵測。

為什麼需要這支
─────────────────────────────────────────────────────────────────────────────
`schema id` 不是全域唯一的。這不是理論上的隱憂，是索引裡實際存在的事實：

    double_pinyin        double-pinyin（繁體詞庫） / ice（簡體詞庫）
    double_pinyin_abc    double-pinyin            / ice
    double_pinyin_flypy  double-pinyin            / ice
    double_pinyin_mspy   double-pinyin            / ice
    pinyin_simp          pinyin-simp              / wubi86-jidian
    radical_pinyin       radical-pinyin           / ice

前四個**字集相反**：一個是繁體、一個是簡體。任何拿 schema id 當唯一鍵的地方
（安裝紀錄、去重、「已安裝」判斷、升級、語言標記對照表）都會在這六個 id 上
給出錯誤答案，而且錯得很安靜 —— 使用者看到的是「我裝的雙拼怎麼變簡體了」。

唯一識別碼的形狀
─────────────────────────────────────────────────────────────────────────────
    uid = <package id> "/" <schema id>

    double-pinyin/double_pinyin      ← 繁體
    ice/double_pinyin                ← 簡體

分隔符取 `/` 是因為兩邊都不可能出現它：套件 id 是 kebab-case（規範 §1「穩定
識別碼，kebab-case」），schema id 是 librime 的檔名主幹（`<id>.schema.yaml`），
含 `/` 的話 librime 自己就找不到檔案。因此 uid 可以用**第一個** `/` 無歧義地
切回兩段，不需要跳脫。

兩個保留命名空間，用 `@` 開頭 —— 套件 id 規定以 `[a-z0-9]` 開頭，撞不到：

    @builtin/<schema id>   隨 APK 出貨的內建方案（不從市集裝，不在 packages 裡）
    @local/<來源>/<schema id>   使用者自帶的 zip／yaml（見下方 local_uid()）

⚠ uid 識別的是「**誰提供了這個方案**」，不是「執行期哪個方案在跑」
─────────────────────────────────────────────────────────────────────────────
librime 執行期只看得到一個攤平的 `user_data_dir` 與一份 `schema_list`，
兩者都用**裸的** schema id。所以 uid 解決的是帳本層的問題，不解決底下這個
更嚴重的問題：

    兩個套件提供同名的 `<id>.schema.yaml`，解壓到同一個目錄會**互相覆蓋**。

實際發生的（見 known_collisions.yaml）：先裝 double-pinyin 再裝 ice，
`double_pinyin.schema.yaml` 被 ice 的版本蓋掉，使用者的自然碼雙拼從繁體
變成簡體，而畫面上什麼都沒說。file_conflicts() 就是為了讓這件事在**安裝前**
講得出來而存在的。
"""

from __future__ import annotations

import re

UID_SEP = "/"
BUILTIN_NS = "@builtin"
LOCAL_NS = "@local"

# 套件 id：規範 §1 說「kebab-case，不可變更」。這裡放寬到允許 . 與 _，
# 但**第一個字元必須是 [a-z0-9]**，好讓 @ 開頭的保留命名空間永遠撞不到。
PKG_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
# schema id：librime 用它組檔名 `<id>.schema.yaml`，所以不可含 / 與空白。
SCHEMA_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]*$")


class UidError(ValueError):
    pass


def check_package_id(pkg_id):
    if not isinstance(pkg_id, str) or not PKG_ID_RE.match(pkg_id):
        raise UidError("套件 id 不合法（需 ^[a-z0-9][a-z0-9._-]*$）：%r" % (pkg_id,))
    return pkg_id


def check_schema_id(schema_id):
    if not isinstance(schema_id, str) or not SCHEMA_ID_RE.match(schema_id):
        raise UidError("方案 id 不合法（需 ^[A-Za-z0-9][A-Za-z0-9._+-]*$）：%r"
                       % (schema_id,))
    return schema_id


def make_uid(pkg_id, schema_id):
    """市集套件提供的方案 → uid。"""
    check_package_id(pkg_id)
    check_schema_id(schema_id)
    return pkg_id + UID_SEP + schema_id


def builtin_uid(schema_id):
    """隨 APK 出貨的內建方案 → uid。"""
    check_schema_id(schema_id)
    return BUILTIN_NS + UID_SEP + schema_id


def local_uid(origin, schema_id):
    """使用者自帶檔案 → uid。

    `origin` 是行動端替那一次匯入取的穩定名字（Android 目前是
    `"local:" + 檔名主幹`）。它是**使用者輸入衍生**的，可能含任何字元，
    所以這裡把 uid 不允許的字元換成 `_`，並保證非空。
    兩次匯入同名檔案會得到同一個 uid —— 那是刻意的：那本來就是「重裝」。
    """
    check_schema_id(schema_id)
    safe = re.sub(r"[^A-Za-z0-9._-]", "_", str(origin or "")).strip("_") or "import"
    return LOCAL_NS + UID_SEP + safe + UID_SEP + schema_id


def parse_uid(uid):
    """uid → (namespace_or_package, schema_id)。

    切在**最後一個** `/`：@local 那條有三段，前兩段合起來才是提供者。
    """
    if not isinstance(uid, str) or UID_SEP not in uid:
        raise UidError("不是合法的 uid：%r" % (uid,))
    owner, _, schema_id = uid.rpartition(UID_SEP)
    if not owner or not schema_id:
        raise UidError("不是合法的 uid：%r" % (uid,))
    return owner, schema_id


def is_builtin_uid(uid):
    return isinstance(uid, str) and uid.startswith(BUILTIN_NS + UID_SEP)


def is_local_uid(uid):
    return isinstance(uid, str) and uid.startswith(LOCAL_NS + UID_SEP)


# ── 撞號偵測 ────────────────────────────────────────────────────────────────

def schema_collisions(packages, builtin=()):
    """→ {schema_id: [提供者, ...]}，只留提供者 >= 2 的。

    `packages` 是 `[{"id":…, "schemas":[{"id":…}, …]}, …]`，
    `builtin`  是 `[{"id": schema_id}, …]`（隨 APK 出貨的內建方案）。

    內建方案也要算進來：它們和市集套件共用同一個扁平命名空間，
    `luna_pinyin` 同時是內建方案與 `luna-pinyin` 套件提供的方案。
    """
    owners = {}
    for p in packages:
        for s in p.get("schemas") or []:
            owners.setdefault(s["id"], []).append(p["id"])
    for b in builtin or ():
        owners.setdefault(b["id"], []).append(BUILTIN_NS)
    return {sid: sorted(set(v)) for sid, v in owners.items() if len(set(v)) > 1}


def package_entries(pkg):
    """套件解壓後會落在 user_data_dir 的相對路徑集合。

    三種來源都吃得下，因為呼叫端有三種：
      · `entries`  data/corpus.json 的脫水語料（已經是 entry 清單）
      · `paths`    analyze.py 的 `{來源檔 → zip entry}`，我們要的是 value
      · `files`    更舊的形式，那時 entry 就等於檔名
    """
    entries = pkg.get("entries")
    if entries:
        return sorted(set(entries))
    paths = pkg.get("paths")
    if isinstance(paths, dict) and paths:
        return sorted(set(paths.values()))
    return sorted(set(pkg.get("files") or []))


def file_conflicts(packages):
    """→ {entry: [套件 id, ...]}，只留 >= 2 個套件都會寫的 entry。

    這是「安裝順序決定使用者拿到什麼」的完整清單。schema_collisions()
    只看得到 `.schema.yaml` 那一層；這裡連 `punctuation.yaml`、
    `key_bindings.yaml` 這種**被所有方案 include 的共用檔**都看得到 ——
    它們被蓋掉的話，影響的是使用者裝的每一個方案，而不只是撞號的那個。
    """
    owners = {}
    for p in packages:
        for e in package_entries(p):
            owners.setdefault(e, []).append(p["id"])
    return {e: sorted(set(v)) for e, v in owners.items() if len(set(v)) > 1}


def conflicts_by_package(packages):
    """→ {套件 id: [{"package":對方, "files":[…], "schemas":[…]}, …]}

    給索引用：每個套件列出「裝了我會蓋掉誰的哪些檔案」。行動端據此在
    安裝前就講得出來，而不是裝完才發現方案內容被換掉。
    """
    fc = file_conflicts(packages)
    sc = schema_collisions(packages)
    out = {}
    for entry, owners in fc.items():
        for a in owners:
            for b in owners:
                if a == b:
                    continue
                out.setdefault(a, {}).setdefault(b, {"files": set(), "schemas": set()})
                out[a][b]["files"].add(entry)
    for sid, owners in sc.items():
        for a in owners:
            for b in owners:
                if a == b or a == BUILTIN_NS or b == BUILTIN_NS:
                    continue
                out.setdefault(a, {}).setdefault(b, {"files": set(), "schemas": set()})
                out[a][b]["schemas"].add(sid)
    return {a: [{"package": b,
                 "files": sorted(v["files"]),
                 "schemas": sorted(v["schemas"])}
                for b, v in sorted(peers.items())]
            for a, peers in sorted(out.items())}
