#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""guid_cross.py — GUID 的三份寫法必須是同一個值。

Windows 端的 GUID 同時以**三種形式**存在,而它們不會互相檢查:

  1. `tsf/guids.cc` 的位元組初始化式  → 編譯器真正用的那一份
  2. 同一個檔案裡上一行的 `// {…}` 註解 → 人在讀的那一份
  3. `verify_installer.sh` 的 EXPECT_CLSID / EXPECT_PROFILES / ARP
     → Windows CI 上拿去比對登錄檔的那一份(刻意寫死的第二意見)

三份分岔的下場各不相同,而且都很難查:
  · (1) 與 (3) 分岔 → install-x64 那個 90 分鐘的 job 在最後才紅;
  · (1) 與 (2) 分岔 → 沒有任何東西會紅。下一個人照著註解去查登錄檔,
    查到的是一個不存在的鍵,然後開始懷疑註冊壞了。

這支把三份都算成同一種正規形式再比。它只讀檔案,不需要 Windows,
所以掛在四分鐘的快速 job 上 —— 分岔在那裡就會被指名。

用法: guid_cross.py <repo 根目錄> <.iss 的相對路徑>
"""
import io
import os
import re
import sys

ROOT = os.path.abspath(sys.argv[1])
ISS_REL = sys.argv[2]

GUIDS_CC = os.path.join(ROOT, "windows", "tsf", "guids.cc")
VERIFY = os.path.join(ROOT, "windows", "verify_installer.sh")
ISS = os.path.join(ROOT, "windows", ISS_REL)

# 我們自己那四個(CLSID + 三份語言設定檔)。其餘的 GUID(顯示屬性、
# 保留鍵、語言列按鈕)不會出現在登錄檔的斷言裡,所以不在這張表上。
WANT = ["CLSID_RimeTextService", "GUID_RimeProfile",
        "GUID_RimeProfileHans", "GUID_RimeProfileHK"]
LANG_OF = {"GUID_RimeProfile": "0x0404",
           "GUID_RimeProfileHans": "0x0804",
           "GUID_RimeProfileHK": "0x0C04"}

problems = []


def canon(a, b, c, tail):
    return "{%08X-%04X-%04X-%s-%s}" % (
        a, b, c,
        "".join("%02X" % t for t in tail[:2]),
        "".join("%02X" % t for t in tail[2:]))


# ── 1. 位元組初始化式 + 上一行的註解 ───────────────────────────────
src = io.open(GUIDS_CC, encoding="utf-8").read()
from_bytes = {}
for sym in WANT:
    m = re.search(
        r"//\s*(\{[0-9A-Fa-f-]{36}\})[^\n]*\n"
        r"extern const (?:CLSID|GUID) " + re.escape(sym) + r"\s*=\s*\{\s*"
        r"0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+),\s*\{([^}]*)\}\s*\}\s*;",
        src)
    if not m:
        problems.append("guids.cc 裡解析不出 %s 的定義" % sym)
        continue
    comment = m.group(1).upper()
    tail = [int(x.strip(), 16) for x in m.group(5).split(",")]
    if len(tail) != 8:
        problems.append("%s 的位元組陣列有 %d 個(需要 8 個)" % (sym, len(tail)))
        continue
    real = canon(int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16), tail)
    from_bytes[sym] = real
    if comment != real:
        problems.append(
            "%s:註解寫 %s,而位元組是 %s —— 照註解去查登錄檔會查到一個不存在的鍵"
            % (sym, comment, real))

# ── 2. verify_installer.sh 的第二意見 ──────────────────────────────
vs = io.open(VERIFY, encoding="utf-8").read()

m = re.search(r"EXPECT_CLSID='(\{[0-9A-Fa-f-]{36}\})'", vs)
if not m:
    problems.append("verify_installer.sh 裡找不到 EXPECT_CLSID")
elif "CLSID_RimeTextService" in from_bytes and \
        m.group(1).upper() != from_bytes["CLSID_RimeTextService"]:
    problems.append(
        "CLSID:guids.cc 是 %s,verify_installer.sh 斷言的是 %s"
        % (from_bytes["CLSID_RimeTextService"], m.group(1).upper()))

expect_profiles = dict(
    (lang, g.upper())
    for lang, g in re.findall(r"'(0x[0-9A-Fa-f]{4})=(\{[0-9A-Fa-f-]{36}\})'", vs))
for sym, lang in LANG_OF.items():
    if sym not in from_bytes:
        continue
    got = expect_profiles.get(lang)
    if got is None:
        problems.append("verify_installer.sh 的 EXPECT_PROFILES 少了 %s" % lang)
    elif got != from_bytes[sym]:
        problems.append(
            "%s(%s):guids.cc 是 %s,verify_installer.sh 斷言的是 %s"
            % (sym, lang, from_bytes[sym], got))

# ── 3. AppId:.iss 與 verify_installer.sh 的 ARP 鍵名 ───────────────
iss = io.open(ISS, encoding="utf-8").read()
m = re.search(r"^AppId=\{(\{[0-9A-Fa-f-]{36}\})$", iss, re.M)
if not m:
    problems.append(".iss 裡解析不出 AppId")
else:
    app_id = m.group(1).upper()
    m2 = re.search(r"Uninstall\\(\{[0-9A-Fa-f-]{36}\})_is1", vs)
    if not m2:
        problems.append("verify_installer.sh 裡找不到 ARP 的鍵名")
    elif m2.group(1).upper() != app_id:
        problems.append(
            "AppId:.iss 是 %s,verify_installer.sh 的 ARP 鍵名是 %s ——"
            "「新增或移除程式」那一項會被判成不存在" % (app_id, m2.group(1).upper()))

if problems:
    for p in problems:
        print("  " + p)
    sys.exit(1)

print("  CLSID、三份語言設定檔、AppId:位元組 / 註解 / 登錄檔斷言三份一致")
for sym in WANT:
    print("    %-24s %s" % (sym, from_bytes.get(sym, "?")))
