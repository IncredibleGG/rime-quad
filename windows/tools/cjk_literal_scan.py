#!/usr/bin/env python3
"""windows/tools/cjk_literal_scan.py — W7:catalog 以外不得有中日韓寬字串字面值

⚠ **這支存在的理由是一次假綠。**

check_ui_spec.sh 的 W7 第一版用的是 docs/ui-design.md §12.2 量字串時那個
慣用法:`grep -o 'L"..."' | grep '[一-鿿]'`。在開發用的 Ubuntu 上,
第二段 grep 回的是:

    grep: Invalid collation character

—— 字元範圍的定序在那個 locale 下不成立。而腳本把 stderr 導去 /dev/null,
於是「錯誤」變成「零個命中」變成「通過」。**W7 從第一天起就是假綠的**,
是反向測試(植入一個 L"測" 之後腳本仍然全綠)把它翻出來的。

這正是 §2-G 講的那個失效方式,只是這一次發生在守門腳本自己身上。
所以這一段改用逐字元的碼點比對:不吃 locale、不吃定序表,
而且「中日韓」到底掃了哪些區間**寫得出來給人看** ——
一個看不懂的字元類是下一次假綠的溫床。

用法:
    cjk_literal_scan.py <windows 目錄>
輸出:
    SCANNED=<掃到的檔案數>
    BAD=<相對路徑>: <字面值>      (每個違規一行,最多 10 行)
"""
import os
import re
import sys

# 中日韓的碼點區間。寫成清單而不是一個 grep 範圍,是為了讓「掃了哪些」
# 讀得出來。
RANGES = [
    (0x3040, 0x30FF),  # 平假名 + 片假名
    (0x3400, 0x4DBF),  # 漢字擴充 A
    (0x4E00, 0x9FFF),  # 漢字基本區
    (0xAC00, 0xD7AF),  # 諺文音節
    (0xF900, 0xFAFF),  # 漢字相容
    (0xFF66, 0xFF9D),  # 半形片假名
]

# ⚠ 唯一的兩個例外,而且兩個都有規範依據:
#   · ui_strings.cc  —— 使用者可見字串的**唯一**歸宿(§12.9.2)。
#   · status_bar.cc  —— §8.12 的四個狀態字面(中/En/简/繁)是四端一致、
#     **不得在地化**的,所以刻意不進 catalog。W10 反過來驗它們**必須**
#     出現在那裡 —— 兩條合起來,這個例外沒有被濫用的空間。
SKIP = {"common/ui_strings.cc", "service/status_bar.cc"}

# ── 第三個例外,而且它有一個**會自己過期**的條件 ────────────────
#
# `tsf/guids.h` 的 RIME_TEXT_SERVICE_DESC 與 `tsf/guids.cc` 的三份語言設定檔
# 描述,是**產品識別碼**不是介面文案:它們被寫進登錄檔,而且
# windows/verify_product_names.sh 會逐列拿它們對帳 `scripts/lib/product.env`
# 的 RS_PRODUCT_NAME_ZH(那支腳本還帶逐列的反向測試)。
#
# 把它們搬進 ui_strings.cc 會造成**兩份真相**,而那正是「改名改一半」
# 這一類事故的來源 —— 上一次改名就是靠那支對帳腳本才發現的。
#
# ⚠ §2-G3:**允許清單不可以活得比它的對象久。**
#   所以這個例外是有條件的:verify_product_names.sh 必須真的還在管
#   guids.h。哪天那條對帳被拿掉,這裡的豁免就自動失效,W7 會把
#   那四個字面值重新報成違規。
GATED_BY_PRODUCT_NAMES = {"tsf/guids.h", "tsf/guids.cc"}


def product_name_gate_still_covers_guids(win):
    """verify_product_names.sh 是不是還在逐列對帳 guids.h。"""
    gate = os.path.join(win, "verify_product_names.sh")
    if not os.path.isfile(gate):
        return False
    text = open(gate, encoding="utf-8", errors="replace").read()
    return "windows/tsf/guids.h" in text and "RIME_TEXT_SERVICE_DESC" in text

LITERAL = re.compile(r'L"(?:[^"\\]|\\.)*"')


def is_cjk(ch):
    o = ord(ch)
    return any(a <= o <= b for a, b in RANGES)


def strip_comments(src):
    """去註解,而且是字串感知的。

    ⚠ 這一步是必要的:這一輪的程式碼裡到處是**解釋為什麼不能那樣寫**的
      註解(例如 cand_window.cc 寫著「這裡原本用 SPI_GETNONCLIENTMETRICS」)。
      不去註解的話,那些說明會被判成違規,而**一條永遠紅的檢查會被關掉**。
    """
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == '"':
            out.append(c)
            i += 1
            while i < n:
                if src[i] == "\\":
                    out.append(src[i:i + 2])
                    i += 2
                    continue
                out.append(src[i])
                if src[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def main():
    if len(sys.argv) < 2:
        print("SCANNED=0")
        return 1
    win = sys.argv[1]
    scanned = 0
    bad = []
    gated = product_name_gate_still_covers_guids(win)
    if not gated:
        # 豁免的前提沒了 —— 那兩個檔案回到掃描範圍裡,而且說清楚為什麼。
        print("BAD=(允許清單失效)verify_product_names.sh 不再逐列對帳 "
              "tsf/guids.h 的 RIME_TEXT_SERVICE_DESC")
    for root, _dirs, files in os.walk(win):
        for fn in sorted(files):
            if not fn.endswith((".cc", ".h")):
                continue
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, win).replace(os.sep, "/")
            if rel in SKIP:
                continue
            if rel in GATED_BY_PRODUCT_NAMES and gated:
                continue
            scanned += 1
            src = open(path, encoding="utf-8", errors="replace").read()
            for m in LITERAL.finditer(strip_comments(src)):
                if any(is_cjk(ch) for ch in m.group(0)):
                    bad.append("%s: %s" % (rel, m.group(0)[:40]))
    print("SCANNED=%d" % scanned)
    for b in bad[:10]:
        print("BAD=" + b)
    return 0


if __name__ == "__main__":
    sys.exit(main())
