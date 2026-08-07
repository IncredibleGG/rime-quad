#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""逐層走一遍當前佈局：進去、量、打字、回來。

每一站都做三件事：
  1. 截圖量列高／列距／總高，與 layout_geom.py（規範算出來的）對照
  2. 按幾顆會送出東西的鍵，確認輸入框真的收到
  3. 走回程鍵，確認回得來

用法: sweep.py <layout> <layer:key,key,...> [<layer:...> ...]
      每一段的 key 清單依序點；`=` 開頭的表示「點完之後量這一層」。
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import layout_drive as L


def stop(layout, layer, taps, typing=None):
    png = L.shot("/tmp/shots/_s.png")
    j = L.geom(layout, layer)
    rows = L.rows_of(png)
    n = j["rows"]
    if len(rows) > n:
        rows = rows[-n:]
    ok = "?"
    if rows:
        total = (rows[-1][1] - rows[0][0] + 1) / L.SCALE + \
            j["padding_dp"]["top"] + j["padding_dp"]["bottom"]
        hs = [round((r[1] - r[0] + 1) / L.SCALE, 2) for r in rows]
        err = abs(total - j["grid_height_dp"]) / j["grid_height_dp"] * 100
        ok = "總高 %.2f dp（規範 %.2f，差 %.2f%%）  列高 %s" % (
            total, j["grid_height_dp"], err, hs)
    print("  [%s/%s] %s" % (layout, layer, ok))
    if typing:
        before = L.field_text()
        ks, _ = L.keys_of(layout, layer)
        for kid in typing:
            if kid not in ks:
                print("      !! 沒有鍵 %s" % kid)
                return
            L.tap(ks[kid])
        after = L.field_text()
        print("      打字：%r → %r  %s" % (before, after, "OK" if after != before else "沒反應!!"))
    if taps:
        ks, _ = L.keys_of(layout, layer)
        for kid in taps:
            if kid not in ks:
                print("      !! 沒有導覽鍵 %s（有：%s）" % (kid, ",".join(sorted(ks))))
                return
            L.tap(ks[kid], wait=1.0)


def main():
    layout = sys.argv[1]
    for seg in sys.argv[2:]:
        layer, _, rest = seg.partition(":")
        nav, typ = [], []
        for tok in [t for t in rest.split(",") if t]:
            (typ if tok.startswith("+") else nav).append(tok.lstrip("+"))
        stop(layout, layer, nav, typ)


main()
