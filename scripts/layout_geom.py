#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
layout_geom.py — 由主題／佈局 YAML 算出每一顆鍵的中心座標。

為什麼要有這支
─────────────────────────────────────────────────────────────────────────────
先前驗證九宮格時，鍵的座標是照 docs/theme-format.md §9.3 用 Python **手算**的。
算對了，但那是一次性的手工活：佈局一改就失效，而 core/layouts/ 現在有十幾份。
verify_input_matrix.sh 走的是另一條路 —— 沿著一欄由下往上逐點戳、看打出什麼字
來反推排距。那條路不必知道佈局，但一次校準要一分鐘，而且只認得出「四排等距」
這一種形狀：九宮格的 5 欄、注音的 11 欄、cn-symbols 的 7 欄它都認不得。

這支把座標變成**算出來的**：讀規範（§8.8.0 高度模型 + §9.3 佈版演算法），
讀主題 yaml、讀佈局 yaml，吐出每顆鍵的矩形。零校準、零寫死、佈局改了自動跟上。

⚠ 公式集中在本檔的 `KeyGeometry.resolve()` 與 `Layer.solve()` 兩個函式裡。
   規範的高度模型若再改（§5.7 已用掉一次例外），只需要動這兩處。
   對照的實作是 android/.../theme/ThemeModel.kt 的 KeyGeometry.resolve
   與 keyboard/KeyboardView.kt 的 KeyGrid，兩邊必須同步。

規範 vs 渲染器：一個必須誠實面對的分歧
─────────────────────────────────────────────────────────────────────────────
§9.3 的列內公式是

    usable_w = kb_width − padding.l − padding.r − key_spacing × (keys.count − 1)
    unit_w   = usable_w / units

但 Android 的渲染器（KeyboardView.KeyGrid）用的是 Compose 的
`Row(horizontalArrangement = spacedBy(key_spacing))` 搭配 `Modifier.weight(width)`，
而且在 `Σwidth < units` 時**額外插入一個 Spacer 子項**把剩餘空間留在列末。
Compose 的 spacedBy 是「每兩個子項之間插一個間距」，所以那顆 Spacer
會多帶來**一個 key_spacing**，間距總數是 `children − 1` 而不是 `keys.count − 1`。

兩者在 `Σwidth == units`（沒有 slack）時完全一致；有 slack 時每顆鍵會差
`key_spacing / units` 左右的寬度，並且整列往左縮。

本檔預設用 **renderer** 語義（`--model renderer`），因為這支腳本的用途是
「戳得中真正畫出來的鍵」；同時提供 `--model spec` 與 `--model both`，
後者會把兩種模型的中心點差異列出來 —— 那正是規範需要補的一句話。

用法
─────────────────────────────────────────────────────────────────────────────
    layout_geom.py --layout qwerty --screen 1080x2400 --density 420
    layout_geom.py --layout cn-t9-pinyin --layer t9 --json
    layout_geom.py --layout bopomofo-dachen --map          # ASCII 鍵位圖
    layout_geom.py --layout qwerty --key a --key space     # 只印這幾顆

座標系：輸出的 x/y 是**像素**，y 以「鍵盤格線區左上角」為原點（--origin 0）。
給 --grid-top <px> 就會加上該偏移，得到螢幕絕對座標。
"""

from __future__ import annotations

import argparse
import json
import os
import sys

try:
    import yaml
except ImportError:  # pragma: no cover
    sys.stderr.write("需要 PyYAML：pip3 install pyyaml\n")
    raise

# ── 規範預設值（§8.8 表格、§8.6.6 表格、§9.1/9.2/9.6 表格）─────────────────
# 這裡刻意把「規範寫的預設」與「主題檔寫的值」分開：主題沒寫的欄位要退回
# 規範預設，而不是退回某一份主題的值。
SPEC_DEFAULTS = {
    "key_aspect": 1.28,
    "key_height_min": 40.0,
    "key_height_max": 56.0,
    "max_screen_ratio_portrait": 0.45,
    "max_screen_ratio_landscape": 0.62,
    "padding": {"left": 5.0, "right": 5.0, "top": 4.0, "bottom": 4.0},
    "row_spacing": 12.0,
    "key_spacing": 6.0,
    "honor_bottom_inset": True,
    "bar_height": 44.0,
    "bar_border_top_width": 0.0,
}

# §8.8 / §8.6.6 的合法區間。超界時規範要求夾制（§6.3 可回復錯誤）。
CLAMPS = {
    "key_aspect": (0.6, 2.5),
    "key_height_min": (20.0, 200.0),
    "key_height_max": (20.0, 200.0),
    "max_screen_ratio_portrait": (0.2, 0.8),
    "max_screen_ratio_landscape": (0.2, 0.9),
    "row_spacing": (0.0, 32.0),
    "key_spacing": (0.0, 32.0),
    "bar_height": (24.0, 96.0),
    "bar_border_top_width": (0.0, 8.0),
    "height_scale": (0.5, 2.0),
    "width": (0.1, 12.0),
    "weight": (0.1, 4.0),
}


def clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def _clamped(name, v):
    lo, hi = CLAMPS[name]
    return clamp(float(v), lo, hi)


# ────────────────────────────── YAML 載入 ──────────────────────────────


def load_yaml(path):
    with open(path, "r", encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def deep_merge(base, over):
    """§7.2 的合併規則：mapping 遞迴合併，其餘型別（含 list）整個取代。"""
    if not isinstance(base, dict) or not isinstance(over, dict):
        return over
    out = dict(base)
    for k, v in over.items():
        out[k] = deep_merge(out.get(k), v) if k in out else v
    return out


def find_doc(doc_id, dirs, kind):
    """§2.3 搜尋路徑：依序在 dirs 裡找 <id>.yaml。"""
    for d in dirs:
        p = os.path.join(d, doc_id + ".yaml")
        if os.path.isfile(p):
            return p
    raise FileNotFoundError("找不到%s '%s'（搜尋路徑：%s）" % (kind, doc_id, ", ".join(dirs)))


def resolve_doc(doc_id, dirs, kind, platform=None, _seen=None):
    """§7.1 `inherits` 鏈 + §8.11 `platform_overrides`。"""
    _seen = _seen or []
    if doc_id in _seen:
        raise ValueError("%s 的 inherits 形成環：%s" % (kind, " → ".join(_seen + [doc_id])))
    path = find_doc(doc_id, dirs, kind)
    doc = load_yaml(path)
    parent_id = doc.get("inherits")
    if parent_id:
        parent = resolve_doc(parent_id, dirs, kind, platform=None, _seen=_seen + [doc_id])
        # §7.3：format / id / inherits 不參與繼承。
        for f in ("format", "id", "inherits", "revision"):
            parent.pop(f, None)
        doc = deep_merge(parent, doc)
    if platform:
        ov = (doc.get("platform_overrides") or {}).get(platform)
        if isinstance(ov, dict) and ov:
            doc = deep_merge(doc, ov)
    doc["__path__"] = path
    return doc


# ────────────────────────────── 主題幾何參數 ──────────────────────────────


class ThemeGeometry(object):
    """從已解析的主題文件抽出高度模型需要的十來個數字。

    刻意只抽這些：本檔不做完整主題解析（顏色、字體、圖示…都與座標無關），
    抽得越少，主題格式演進時這支腳本要跟著改的面積越小。
    """

    def __init__(self, theme_doc, theme_id="<inline>"):
        kb = theme_doc.get("keyboard") or {}
        bar = ((theme_doc.get("candidates") or {}).get("bar")) or {}
        d = SPEC_DEFAULTS
        self.theme_id = theme_id
        self.aspect = _clamped("key_aspect", kb.get("key_aspect", d["key_aspect"]))
        kh = kb.get("key_height") or {}
        self.key_h_min = _clamped("key_height_min", kh.get("min", d["key_height_min"]))
        self.key_h_max = _clamped("key_height_max", kh.get("max", d["key_height_max"]))
        msr = kb.get("max_screen_ratio") or {}
        self.ratio_portrait = _clamped(
            "max_screen_ratio_portrait", msr.get("portrait", d["max_screen_ratio_portrait"])
        )
        self.ratio_landscape = _clamped(
            "max_screen_ratio_landscape", msr.get("landscape", d["max_screen_ratio_landscape"])
        )
        pad = kb.get("padding")
        if isinstance(pad, (int, float)):      # §4.3：單一數字 = 四邊相同
            pad = {k: float(pad) for k in ("left", "right", "top", "bottom")}
        pad = pad or {}
        self.pad = {k: float(pad.get(k, d["padding"][k])) for k in ("left", "right", "top", "bottom")}
        self.row_spacing = _clamped("row_spacing", kb.get("row_spacing", d["row_spacing"]))
        self.key_spacing = _clamped("key_spacing", kb.get("key_spacing", d["key_spacing"]))
        self.honor_bottom_inset = bool(kb.get("honor_bottom_inset", d["honor_bottom_inset"]))
        self.bar_height = _clamped("bar_height", bar.get("height", d["bar_height"]))
        self.bar_border = _clamped(
            "bar_border_top_width", bar.get("border_top_width", d["bar_border_top_width"])
        )
        # §8.8.0.2：舊的 height: 區塊必須被忽略，只留一則 INFO。
        self.legacy_height_block = "height" in kb

    def resolve(self, width_dp, avail_h_dp, landscape, units, rows_weight, row_count,
                key_spacing, row_spacing, height_scale=1.0):
        """§8.8.0 的高度模型：鍵寬 → 鍵高 → 鍵盤高。

        與 ThemeModel.kt 的 KeyGeometry.resolve 逐行對應。改這裡就要改那裡。
        """
        units = float(units) if units > 0 else 1.0
        rows_weight = float(rows_weight) if rows_weight > 0 else 1.0
        gaps = row_count - 1 if row_count > 1 else 0

        # 1. 鍵寬：由可用寬度、欄數、鍵距決定。與螢幕高度無關。
        inner_w = width_dp - self.pad["left"] - self.pad["right"] - key_spacing * (units - 1.0)
        key_w = (inner_w if inner_w > 0 else 0.0) / units

        # 2. 鍵高：鍵寬 × aspect，夾制到絕對上下界之後才套 height_scale。
        key_h = clamp(key_w * self.aspect, self.key_h_min, self.key_h_max) * height_scale

        # 3. 鍵盤高度是算出來的結果，不是設定值。
        chrome = row_spacing * gaps + self.pad["top"] + self.pad["bottom"]
        h = key_h * rows_weight + chrome

        # 4. 安全網：不得超過螢幕的這個比例。
        ratio = self.ratio_landscape if landscape else self.ratio_portrait
        cap = avail_h_dp * ratio
        capped = False
        if avail_h_dp > 0 and h > cap:
            key_h = max((cap - chrome) / rows_weight, 1.0)
            h = key_h * rows_weight + chrome
            capped = True
        return {
            "key_width_dp": key_w,          # 只是「名目鍵寬」，用來推鍵高；不是實際排版寬度
            "key_height_dp": key_h,         # 名目鍵高，weight=1.0 的列才等於實際列高
            "keyboard_height_dp": h,
            "capped": capped,
        }


# ────────────────────────────── 佈局模型 ──────────────────────────────


class Key(object):
    def __init__(self, raw, index):
        if not isinstance(raw, dict):
            raw = {}
        self.raw = raw
        self.index = index
        self.id = raw.get("id")
        self.label = raw.get("label", "") or ""
        self.icon = raw.get("icon")
        self.spacer = bool(raw.get("spacer", False))
        self.width = _clamped("width", raw.get("width", 1.0))
        self.send = raw.get("send")
        self.tap = raw.get("tap")

    @property
    def display(self):
        """給鍵位圖用的短標籤。不是渲染規則（§9.6 的解析順序在 app 裡），
        只是為了讓人在除錯輸出裡認得出這顆是哪一顆。"""
        if self.spacer:
            return "·"
        for cand in (self.label, self.icon, self.id):
            if cand:
                return str(cand)
        if isinstance(self.send, dict):
            return str(self.send.get("keysym") or self.send.get("text") or "?")
        return "?"

    @property
    def addressable(self):
        return (not self.spacer) and bool(self.id)


class Row(object):
    def __init__(self, raw, index):
        self.index = index
        self.weight = _clamped("weight", (raw or {}).get("weight", 1.0))
        self.keys = [Key(k, i) for i, k in enumerate((raw or {}).get("keys") or [])]

    @property
    def width_sum(self):
        return sum(k.width for k in self.keys)


class Layer(object):
    def __init__(self, raw):
        self.id = (raw or {}).get("id") or ""
        self.rows = [Row(r, i) for i, r in enumerate((raw or {}).get("rows") or [])]
        declared = (raw or {}).get("units")
        # §9.3：units 預設為各 row width 總和的最大值。
        self.units = float(declared) if declared else max(
            [r.width_sum for r in self.rows] or [1.0]
        )

    @property
    def rows_weight(self):
        return sum(r.weight for r in self.rows)


class Layout(object):
    def __init__(self, doc, layout_id):
        self.id = doc.get("id") or layout_id
        self.path = doc.get("__path__")
        self.default_layer = doc.get("default_layer")
        self.for_schema = doc.get("for_schema") or ["*"]
        self.primary = bool(doc.get("primary", False))
        self.direction = doc.get("direction", "ltr")
        m = doc.get("metrics") or {}
        self.row_spacing = m.get("row_spacing")
        self.key_spacing = m.get("key_spacing")
        hs = m.get("height_scale")
        self.height_scale = _clamped("height_scale", hs) if hs is not None else 1.0
        self.layers = [Layer(l) for l in (doc.get("layers") or [])]
        if not self.default_layer and self.layers:
            self.default_layer = self.layers[0].id
        # §9.7 key_patches：只影響鍵面與行為，不影響 width/spacer 以外的幾何。
        for kid, patch in (doc.get("key_patches") or {}).items():
            if not isinstance(patch, dict):
                continue
            for layer in self.layers:
                for row in layer.rows:
                    for key in row.keys:
                        if key.id == kid:
                            key.raw = deep_merge(key.raw, patch)
                            if "width" in patch:
                                key.width = _clamped("width", patch["width"])
                            if "spacer" in patch:
                                key.spacer = bool(patch["spacer"])
                            if "label" in patch:
                                key.label = patch["label"] or ""

    def layer(self, layer_id=None):
        want = layer_id or self.default_layer
        for l in self.layers:
            if l.id == want:
                return l
        if layer_id:
            raise KeyError(
                "佈局 %s 沒有層 '%s'（有：%s）"
                % (self.id, layer_id, ", ".join(l.id for l in self.layers))
            )
        return self.layers[0]


# ────────────────────────────── 佈版求解 ──────────────────────────────


def solve(theme, layout, layer, screen_w_px, screen_h_px, density,
          landscape=False, model="renderer", avail_h_dp=None):
    """算出這一層每顆鍵的矩形。回傳 dp 與 px 兩套數字。

    y 的原點是**鍵盤格線區的左上角**（不含候選列、不含底部 inset）。
    絕對座標由呼叫端加上 grid_top 得到 —— 那個偏移量與佈局無關，
    量一次（或由 dumpsys 讀）就能重複使用，不該混進佈版演算法裡。
    """
    scale = density / 160.0
    width_dp = screen_w_px / scale
    height_dp = screen_h_px / scale
    if avail_h_dp is None:
        avail_h_dp = height_dp

    key_spacing = layout.key_spacing if layout.key_spacing is not None else theme.key_spacing
    row_spacing = layout.row_spacing if layout.row_spacing is not None else theme.row_spacing
    key_spacing = _clamped("key_spacing", key_spacing)
    row_spacing = _clamped("row_spacing", row_spacing)

    g = theme.resolve(
        width_dp=width_dp,
        avail_h_dp=avail_h_dp,
        landscape=landscape,
        units=layer.units,
        rows_weight=layer.rows_weight,
        row_count=len(layer.rows),
        key_spacing=key_spacing,
        row_spacing=row_spacing,
        height_scale=layout.height_scale,
    )

    kb_h = g["keyboard_height_dp"]
    pad = theme.pad
    warnings = []

    # ── 列高（§9.3）──────────────────────────────────────────────────────
    n_rows = len(layer.rows)
    usable_h = kb_h - pad["top"] - pad["bottom"] - row_spacing * (n_rows - 1 if n_rows else 0)
    total_w = layer.rows_weight or 1.0
    row_rects = []
    y = pad["top"]
    for row in layer.rows:
        h = usable_h * row.weight / total_w
        row_rects.append((y, h))
        y += h + row_spacing

    # ── 列內（§9.3 + 渲染器的 Compose 語義）──────────────────────────────
    inner_w = width_dp - pad["left"] - pad["right"]
    keys_out = []
    for row, (ry, rh) in zip(layer.rows, row_rects):
        slack = layer.units - row.width_sum
        if row.width_sum > layer.units + 1e-4:
            warnings.append(
                "第 %d 列 Σwidth=%.3f > units=%.3f，該列會等比壓縮（§9.3 WARNING）"
                % (row.index, row.width_sum, layer.units)
            )
        if model == "spec":
            # §9.3 的字面公式：間距數 = keys.count − 1，slack 不佔間距。
            n_gaps = max(len(row.keys) - 1, 0)
            unit_w = (inner_w - key_spacing * n_gaps) / max(layer.units, row.width_sum)
            children = list(row.keys)
            child_widths = [k.width * unit_w for k in children]
        else:
            # 渲染器（KeyboardView.KeyGrid）：Compose Row + spacedBy + weight。
            # slack > 0 時多一顆 Spacer 子項，因此多一個 key_spacing。
            children = list(row.keys)
            weights = [k.width for k in children]
            if slack > 0.01:
                children = children + [None]          # 尾端的 slack Spacer
                weights = weights + [slack]
            n_gaps = max(len(children) - 1, 0)
            weight_space = inner_w - key_spacing * n_gaps
            total = sum(weights) or 1.0
            child_widths = [weight_space * w / total for w in weights]

        x = pad["left"]
        for child, w in zip(children, child_widths):
            if child is not None:
                keys_out.append({
                    "layer": layer.id,
                    "row": row.index,
                    "col": child.index,
                    "id": child.id,
                    "label": child.display,
                    "spacer": child.spacer,
                    "width_units": child.width,
                    "x_dp": x, "y_dp": ry, "w_dp": w, "h_dp": rh,
                    "cx_dp": x + w / 2.0, "cy_dp": ry + rh / 2.0,
                })
            x += w + key_spacing

    for k in keys_out:
        k["x"] = int(round(k["x_dp"] * scale))
        k["y"] = int(round(k["y_dp"] * scale))
        k["w"] = int(round(k["w_dp"] * scale))
        k["h"] = int(round(k["h_dp"] * scale))
        k["cx"] = int(round(k["cx_dp"] * scale))
        k["cy"] = int(round(k["cy_dp"] * scale))

    if theme.legacy_height_block:
        warnings.append("主題含已被取代的 keyboard.height: 區塊，已忽略（§8.8.0.2 INFO）")
    if g["capped"]:
        warnings.append(
            "鍵盤高度撞到 max_screen_ratio 安全網，key_aspect 已讓位（§8.8.0 第 4 步）"
        )

    return {
        "layout": layout.id,
        "layer": layer.id,
        "units": layer.units,
        "rows": len(layer.rows),
        "density": density,
        "scale": scale,
        "screen_px": [screen_w_px, screen_h_px],
        "screen_dp": [width_dp, height_dp],
        "model": model,
        "key_spacing_dp": key_spacing,
        "row_spacing_dp": row_spacing,
        "padding_dp": pad,
        "height_scale": layout.height_scale,
        "geometry": g,
        # 這三個高度是絕對座標換算的全部依據，故一併輸出：
        #   grid_h  鍵盤格線區高（§8.8.0 的 h）
        #   bar_h   候選列總高（bar.height + border_top_width，**外加**在 h 之上）
        #   兩者相加再加上底部 inset 就是 IME 視窗高（§8.8.0 的 total）
        "grid_height_dp": kb_h,
        "grid_height_px": int(round(kb_h * scale)),
        "bar_height_dp": theme.bar_height + theme.bar_border,
        "bar_height_px": int(round((theme.bar_height + theme.bar_border) * scale)),
        "honor_bottom_inset": theme.honor_bottom_inset,
        "keys": keys_out,
        "warnings": warnings,
    }


def key_index(sol):
    """id → 鍵。同 id 只留第一顆（§9.6：id 在 layer 內建議唯一）。"""
    out = {}
    for k in sol["keys"]:
        if k["id"] and not k["spacer"] and k["id"] not in out:
            out[k["id"]] = k
    return out


def ascii_map(sol, origin_y=0):
    """把算出來的鍵位攤成一張人看得懂的圖。失敗時附在報告裡就知道戳到哪。"""
    lines = []
    lines.append(
        "佈局 %s / 層 %s   units=%g  rows=%d  模型=%s"
        % (sol["layout"], sol["layer"], sol["units"], sol["rows"], sol["model"])
    )
    lines.append(
        "螢幕 %dx%d px @%ddpi (%.1fx%.1f dp)   格線區高 %.2f dp / %d px   候選列 %d px"
        % (sol["screen_px"][0], sol["screen_px"][1], sol["density"],
           sol["screen_dp"][0], sol["screen_dp"][1],
           sol["grid_height_dp"], sol["grid_height_px"], sol["bar_height_px"])
    )
    lines.append(
        "名目鍵 %.2f x %.2f dp   key_spacing %.1f  row_spacing %.1f  height_scale %g"
        % (sol["geometry"]["key_width_dp"], sol["geometry"]["key_height_dp"],
           sol["key_spacing_dp"], sol["row_spacing_dp"], sol["height_scale"])
    )
    if origin_y:
        lines.append("y 原點：螢幕絕對座標，格線區頂端 = %d px" % origin_y)
    else:
        lines.append("y 原點：格線區左上角（加上 --grid-top 才是螢幕絕對座標）")
    lines.append("")
    lines.append("%-4s %-4s %-14s %8s %8s %7s %7s  %s"
                 % ("列", "欄", "id", "cx", "cy", "w", "h", "鍵面"))
    lines.append("-" * 78)
    last_row = None
    for k in sol["keys"]:
        if last_row is not None and k["row"] != last_row:
            lines.append("")
        last_row = k["row"]
        lines.append(
            "%-4d %-4d %-14s %8d %8d %7d %7d  %s%s"
            % (k["row"], k["col"], (k["id"] or "-")[:14], k["cx"], k["cy"] + origin_y,
               k["w"], k["h"], k["label"], "  (spacer)" if k["spacer"] else "")
        )
    for w in sol["warnings"]:
        lines.append("⚠ " + w)
    return "\n".join(lines)


# ────────────────────────────── CLI ──────────────────────────────


def build(args):
    root = args.root
    layout_dirs = [d for d in (args.layout_dir or []) if d] or [os.path.join(root, "core", "layouts")]
    theme_dirs = [d for d in (args.theme_dir or []) if d] or [os.path.join(root, "core", "themes")]

    theme_doc = resolve_doc(args.theme, theme_dirs, "主題", platform=args.platform)
    theme = ThemeGeometry(theme_doc, args.theme)
    layout_doc = resolve_doc(args.layout, layout_dirs, "佈局", platform=args.platform)
    layout = Layout(layout_doc, args.layout)
    layer = layout.layer(args.layer)

    w, _, h = args.screen.partition("x")
    return solve(
        theme, layout, layer,
        screen_w_px=int(w), screen_h_px=int(h), density=float(args.density),
        landscape=args.landscape, model=args.model,
        avail_h_dp=(float(args.avail_dp) if args.avail_dp else None),
    )


def main(argv=None):
    ap = argparse.ArgumentParser(description="由佈局 YAML 算出每顆鍵的中心座標")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--layout", required=True)
    ap.add_argument("--layer", default=None, help="預設用佈局的 default_layer")
    ap.add_argument("--theme", default="default-light")
    ap.add_argument("--layout-dir", action="append")
    ap.add_argument("--theme-dir", action="append")
    ap.add_argument("--platform", default="android")
    ap.add_argument("--screen", default="1080x2400", help="WxH 像素")
    ap.add_argument("--density", default="420", help="dpi（dp = px * 160 / density）")
    ap.add_argument("--avail-dp", default=None, help="安全網用的可用高度 dp（預設 = 螢幕高）")
    ap.add_argument("--landscape", action="store_true")
    ap.add_argument("--model", default="renderer", choices=["renderer", "spec", "both"])
    ap.add_argument("--grid-top", type=int, default=0, help="格線區頂端的螢幕 y（px）")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--map", action="store_true")
    ap.add_argument("--key", action="append", help="只印這些 id 的 '<cx> <cy>'")
    args = ap.parse_args(argv)

    if args.model == "both":
        args.model = "renderer"
        a = build(args)
        args.model = "spec"
        b = build(args)
        ia, ib = key_index(a), key_index(b)
        print("模型比對：renderer vs spec（§9.3 字面公式）")
        print("%-16s %8s %8s %8s" % ("id", "renderer", "spec", "差"))
        worst = 0
        for kid in ia:
            if kid in ib:
                d = ia[kid]["cx"] - ib[kid]["cx"]
                worst = max(worst, abs(d))
                if d:
                    print("%-16s %8d %8d %8+d" % (kid, ia[kid]["cx"], ib[kid]["cx"], d))
        print("最大差異 %d px" % worst)
        return 0

    sol = build(args)

    if args.key:
        idx = key_index(sol)
        for kid in args.key:
            k = idx.get(kid)
            if not k:
                sys.stderr.write("佈局 %s 層 %s 沒有 id 為 '%s' 的可點鍵\n"
                                 % (sol["layout"], sol["layer"], kid))
                return 3
            print("%d %d" % (k["cx"], k["cy"] + args.grid_top))
        return 0

    if args.json:
        sol["grid_top_px"] = args.grid_top
        for k in sol["keys"]:
            k["screen_cy"] = k["cy"] + args.grid_top
        print(json.dumps(sol, ensure_ascii=False, indent=2))
        return 0

    print(ascii_map(sol, origin_y=args.grid_top))
    return 0


if __name__ == "__main__":
    sys.exit(main())
