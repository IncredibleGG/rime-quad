#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""bar_items.py — 候選列上每一格候選的 x 範圍(靠墨跡分群,不靠座標公式)。

為什麼不用公式算
─────────────────────────────────────────────────────────────────────────────
`verify_selection_digit.sh` 要點「第 3 個候選」,而它同時在驗的正是那個公式
(有沒有序號 → 一格多寬)。拿被驗的東西去定位被驗的東西,那是同義反覆:
公式錯的時候點到別格,而「點錯地方」與「按了沒用」在輸出上長得一模一樣。

所以位置從**畫面**來:候選列上一格與一格之間的空白是
`item.spacing + 2 × item.padding_h`(隨附主題是 4 + 16 = 20 dp ≈ 52 px),
而一格**內部**最大的空白是序號與候選字之間的 `GAP_DP = 3 dp ≈ 8 px`。
兩者差了六倍,取中間當門檻就分得開。

⚠ 這一支只回「每一團墨跡在哪」,不回「那是第幾個候選」的語意。呼叫端要自己
  確認第一團是不是候選(左端可能有行內組字串)—— 所以它接受一個 x 下界。

用法
─────────────────────────────────────────────────────────────────────────────
    bar_items.py <png> <y0> <y1> [x_min] [gap_px]
        → 一行一團:`<x0> <x1> <中心x>`
"""
from __future__ import annotations

import sys

from PIL import Image


def clusters(path: str, y0: int, y1: int, x_min: int = 0, gap: int = 26):
    im = Image.open(path).convert("RGB")
    y0 = max(0, min(y0, im.height - 2))
    y1 = max(y0 + 2, min(y1, im.height))
    crop = im.crop((0, y0, im.width, y1))
    px = crop.load()
    w, h = crop.size
    # 背景色取自帶子的左上角一小塊的中位數:候選列自己的底色隨主題而變,
    # 寫死「白」在深色主題上會把整條帶子都當成墨跡。
    ref = px[2, 2]
    ink_cols = []
    for x in range(x_min, w):
        hit = 0
        for y in range(1, h - 1):
            r, g, b = px[x, y]
            if abs(r - ref[0]) + abs(g - ref[1]) + abs(b - ref[2]) > 90:
                hit += 1
                if hit >= 2:
                    break
        if hit >= 2:
            ink_cols.append(x)
    out = []
    if not ink_cols:
        return out
    start = prev = ink_cols[0]
    for x in ink_cols[1:]:
        if x - prev > gap:
            out.append((start, prev))
            start = x
        prev = x
    out.append((start, prev))
    return out


def main() -> int:
    if len(sys.argv) < 4:
        sys.stderr.write(__doc__ or "")
        return 2
    path = sys.argv[1]
    y0, y1 = int(sys.argv[2]), int(sys.argv[3])
    x_min = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    gap = int(sys.argv[5]) if len(sys.argv) > 5 else 26
    for a, b in clusters(path, y0, y1, x_min, gap):
        print("%d %d %d" % (a, b, (a + b) // 2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
