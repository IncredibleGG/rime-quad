#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""所有隨附佈局在同一台裝置上的高度／長寬比對照表。

存在的理由：§8.8.0 的核心保證是「同一份主題下，任兩份佈局的鍵盤總高相同」。
那是一句跨檔案的斷言 —— 單一佈局的截圖看不出來，必須把十幾份並排。
競品對照用的長寬比（平均鍵寬 ÷ 列高）也在這裡一次算完。

用法: layout_geom_table.py [主題id ...]
"""
import glob, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import layout_geom as G

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TDIR = [os.path.join(ROOT, "core", "themes")]
LDIR = [os.path.join(ROOT, "core", "layouts")]
W, H, D = 1440, 3120, 505


def table(theme_id):
    theme = G.ThemeGeometry(G.resolve_doc(theme_id, TDIR, "主題", platform="android"), theme_id)
    print("=" * 96)
    print("主題 %s   螢幕 %dx%d @%ddpi = %.1f x %.1f dp"
          % (theme_id, W, H, D, W / (D / 160.0), H / (D / 160.0)))
    print("%-26s %-10s %4s %7s %8s %8s %9s %6s" %
          ("佈局", "層", "欄", "Σweight", "總高dp", "列高dp", "均鍵寬dp", "w/h"))
    print("-" * 96)
    heights = []
    for f in sorted(glob.glob(os.path.join(LDIR[0], "*.yaml"))):
        lid = os.path.basename(f)[:-5]
        lay = G.Layout(G.resolve_doc(lid, LDIR, "佈局", platform="android"), lid)
        for layer in lay.layers:
            r = G.solve(theme, lay, layer, W, H, D)
            g = r["geometry"]
            kb = r["grid_height_dp"]
            # 平均鍵寬取「鍵數最多的那一列」，與截圖量法一致
            best = None
            for row in layer.rows:
                ks = [k for k in row.keys if not k.spacer]
                if best is None or len(ks) > len(best):
                    best = ks
            n = max(len(best), 1)
            avail = (W / (D / 160.0) - r["padding_dp"]["left"] - r["padding_dp"]["right"]
                     - r["key_spacing_dp"] * (n - 1))
            avg_w = avail / n
            rh = g["key_height_dp"]
            heights.append(kb)
            print("%-26s %-10s %4.4g %7.2f %8.2f %8.2f %9.2f %6.2f"
                  % (lid, layer.id, layer.units, layer.rows_weight, kb, rh, avg_w, avg_w / rh))
    print("-" * 96)
    lo, hi = min(heights), max(heights)
    print("鍵盤總高：最小 %.2f dp，最大 %.2f dp，最大差異 %.2f%%"
          % (lo, hi, (hi - lo) / lo * 100.0))
    print()


for t in (sys.argv[1:] or ["default-light", "cn-compact-light"]):
    table(t)
