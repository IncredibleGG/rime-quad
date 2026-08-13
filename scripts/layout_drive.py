#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在模擬器上實際操作佈局：逐鍵點、逐層進出、讀回輸入框內容。

存在的理由
─────────────────────────────────────────────────────────────────────────────
單元測試守得住「佈局檔的導覽圖沒有死路」，守不住「這顆鍵畫在螢幕上的那個位置
真的按得到、按下去真的送出東西」。那需要在真裝置（或模擬器）上按。

座標由 layout_geom.py（規範算出來的）提供，螢幕上的 y 原點由截圖量到的
「鍵盤格線區上緣」反推 —— 不寫死任何 offset，換佈局／主題／螢幕都自動跟上。

用法：
    layout_drive.py measure <layout> [layer]     只量幾何，印出對照數字
    layout_drive.py type <layout> <layer> <k1,k2,...>   逐鍵點並讀回輸入框
    layout_drive.py shot <out.png>
"""
import json
import os
import re
import subprocess
import sys
import time
from collections import Counter

ADB = os.path.expanduser("~/Android/Sdk/platform-tools/adb")
# ⛔ 沒有預設序號。這一行從前寫死 `emulator-5574` —— 那台現在根本不存在,
#    而「猜一個不存在的序號」的失敗訊息與「產品壞了」長得不一樣,
#    「猜一個**存在但是別人的**序號」才是真正的危險。
SERIAL = os.environ.get("RIME_SERIAL") or os.environ.get("ANDROID_SERIAL")
if not SERIAL:
    sys.stderr.write("要指定 RIME_SERIAL=emulator-XXXX —— 這台機器上不只一台模擬器,不猜。\n")
    sys.exit(2)
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W = int(os.environ.get("RIME_W", "1440"))
H = int(os.environ.get("RIME_H", "3120"))
D = float(os.environ.get("RIME_D", "505"))
SCALE = D / 160.0


def shell(cmd):
    return subprocess.run([ADB, "-s", SERIAL, "shell", cmd],
                          capture_output=True, text=True).stdout


def shot(path):
    out = subprocess.run([ADB, "-s", SERIAL, "exec-out", "screencap", "-p"],
                         capture_output=True)
    with open(path, "wb") as f:
        f.write(out.stdout)
    return path


def field_text():
    shell("uiautomator dump /sdcard/u.xml >/dev/null 2>&1")
    xml = shell("cat /sdcard/u.xml")
    for pat in (r"text=\"([^\"]*)\"[^>]*content-desc=\"rime_test_input\"",
                r"content-desc=\"rime_test_input\"[^>]*text=\"([^\"]*)\""):
        m = re.search(pat, xml)
        if m:
            return m.group(1)
    return "<?>"


def near(a, b, t=10):
    return all(abs(a[i] - b[i]) <= t for i in range(3))


def _bg_frac(png):
    """回傳 (底色, 每一 y 的底色佔比)。底色 = 畫面下三分之一最常見、且橫貫整寬的色。"""
    from PIL import Image
    im = Image.open(png).convert("RGB")
    px = im.load()
    lo = int(H * 0.72)
    c = Counter()
    for y in range(lo, H, 2):
        for x in range(0, W, 3):
            c[px[x, y]] += 1
    best = None
    for bg, _ in c.most_common(5):
        frac = [sum(1 for x in range(0, W, 4) if near(px[x, y], bg)) / (W // 4 + 1)
                for y in range(H)]
        # 判準不是「這個顏色多不多」，而是「它有沒有排成一條條細的橫帶」——
        # 那些橫帶就是列與列之間的空隙。宿主畫面的大片底色不會長這樣。
        gaps, s0 = 0, None
        for y in range(int(H * 0.55), H):
            if frac[y] > 0.90 and s0 is None:
                s0 = y
            elif frac[y] <= 0.90 and s0 is not None:
                if 15 <= y - s0 <= 70:
                    gaps += 1
                s0 = None
        if gaps >= 2 and (best is None or gaps > best[0]):
            best = (gaps, bg, frac)
    if best is None:
        return None, None, None
    return best[1], best[2], px


def grid_top(png, n_rows=None, pad_top_dp=4.0):
    """鍵盤格線區上緣（px）。

    **不能**取「底色橫貫整寬的最上緣」—— 候選列的底色與鍵盤底色相同（都是
    `$bg`），那樣量到的是候選列的上緣，會整整差半列，點下去全部落在上一列。
    正確的錨點是**第一列鍵的上緣**，往上退一個 `padding.top`。
    """
    rows = rows_of(png)
    if not rows:
        return None
    if n_rows and len(rows) > n_rows:
        rows = rows[-n_rows:]
    return int(round(rows[0][0] - pad_top_dp * SCALE))


def rows_of(png):
    """量出每一列的 (上緣, 下緣) 與列距，全部是 px。"""
    bg, frac, px = _bg_frac(png)
    if frac is None:
        return []
    lo = int(H * 0.55)
    gapmask = [y >= lo and frac[y] > 0.90 for y in range(H)]
    gaps, s = [], None
    for y, v in enumerate(gapmask):
        if v and s is None:
            s = y
        elif not v and s is not None:
            if 12 <= y - s <= 70:
                gaps.append((s, y - 1))
            s = None
    if len(gaps) < 2:
        return []
    rows = [(gaps[i][1] + 1, gaps[i + 1][0] - 1) for i in range(len(gaps) - 1)]
    # 首列：由第一個空隙往上走到底色結束
    a = gaps[0][0] - 1
    while a > 0 and frac[a] < 0.90:
        a -= 1
    rows.insert(0, (a + 1, gaps[0][0] - 1))
    # 末列：由最後一個空隙往下走
    b = gaps[-1][1] + 1
    while b < H - 1 and frac[b] < 0.90:
        b += 1
    rows.append((gaps[-1][1] + 1, b - 1))
    return [r for r in rows if r[1] - r[0] + 1 >= 20]


def geom(layout_id, layer_id=None):
    args = [sys.executable, os.path.join(ROOT, "scripts", "layout_geom.py"),
            "--layout", layout_id,
            "--screen", "%dx%d" % (W, H), "--density", str(int(D)), "--json"]
    if layer_id:
        args += ["--layer", layer_id]
    out = subprocess.run(args, capture_output=True, text=True)
    return json.loads(out.stdout)


def keys_of(layout_id, layer_id, png=None):
    """鍵 id → 螢幕絕對座標。y 原點每次都由當下的截圖重新量。"""
    j = geom(layout_id, layer_id)
    png = png or shot("/tmp/shots/_t.png")
    top = grid_top(png, j["rows"], j["padding_dp"]["top"])
    if top is None:
        raise SystemExit("量不到鍵盤格線區上緣")
    return {k["id"]: (k["cx"], k["cy"] + top) for k in j["keys"] if k.get("id")}, top


def tap(xy, wait=0.5):
    shell("input tap %d %d" % (xy[0], xy[1]))
    time.sleep(wait)


def clear_field():
    shell("input keyevent KEYCODE_MOVE_END")
    for _ in range(40):
        shell("input keyevent KEYCODE_DEL")


def cmd_measure(layout, layer=None):
    png = shot("/tmp/shots/_m.png")
    rows = rows_of(png)
    j = geom(layout, layer)
    top = grid_top(png, j["rows"], j["padding_dp"]["top"])
    g = j["geometry"]
    print("佈局 %s / 層 %s" % (j["layout"], j["layer"]))
    print("  規範算出：總高 %.2f dp（預算 %.2f）、列高 %.2f dp"
          % (j["grid_height_dp"], g["budget_height_dp"], g["key_height_dp"]))
    if not rows:
        print("  截圖量不到列")
        return
    # 候選列（bar）也是一條橫帶，會排在鍵列之前。鍵列數由佈局決定，取最後 n 條。
    n = j["rows"]
    if len(rows) > n:
        rows = rows[-n:]
    hs = [r[1] - r[0] + 1 for r in rows]
    print("  截圖量到：%d 列，列高 %s px = %s dp"
          % (len(rows), hs, [round(h / SCALE, 2) for h in hs]))
    gaps = [rows[i + 1][0] - rows[i][1] - 1 for i in range(len(rows) - 1)]
    print("            列距 %s px = %s dp" % (gaps, [round(x / SCALE, 2) for x in gaps]))
    total_px = rows[-1][1] - rows[0][0] + 1
    pad = j["padding_dp"]["top"] + j["padding_dp"]["bottom"]
    print("            鍵區 %d px = %.2f dp，加 padding %.0f dp → 總高 %.2f dp"
          % (total_px, total_px / SCALE, pad, total_px / SCALE + pad))
    print("  格線區上緣 y = %s px" % top)
    err = abs(total_px / SCALE + pad - j["grid_height_dp"]) / j["grid_height_dp"] * 100.0
    print("  規範 vs 截圖 誤差 %.2f%%" % err)


def main():
    os.makedirs("/tmp/shots", exist_ok=True)
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return
    if a[0] == "measure":
        cmd_measure(a[1], a[2] if len(a) > 2 else None)
    elif a[0] == "shot":
        print(shot(a[1]))
    elif a[0] == "top":
        print(grid_top(shot("/tmp/shots/_t.png")))
    elif a[0] == "text":
        print(field_text())
    elif a[0] == "clear":
        clear_field()
    elif a[0] == "type":
        layout, layer, seq = a[1], a[2], a[3]
        ks, top = keys_of(layout, layer)
        for kid in seq.split(","):
            kid = kid.strip()
            if kid not in ks:
                print("  !! 這一層沒有鍵 %s（有：%s）" % (kid, ",".join(sorted(ks))))
                sys.exit(2)
            tap(ks[kid])
        print(field_text())
    elif a[0] == "tapkey":
        layout, layer, kid = a[1], a[2], a[3]
        ks, top = keys_of(layout, layer)
        tap(ks[kid], wait=1.0)
        print("tapped %s at %s" % (kid, ks[kid]))
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
