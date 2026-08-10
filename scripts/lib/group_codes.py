#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
group_codes.py — 一份佈局裡，哪些字元是「一整組字母的代號」。

為什麼要有這支
─────────────────────────────────────────────────────────────────────────────
九宮格是雙編碼方案：`mno` 那顆鍵送出去的是代表字母 `M`。於是 librime 的
preedit 是 `MG GAM` / `PGM` —— 那是內部編碼，不是使用者的語言，而它一路送進
了宿主 app 的輸入框（使用者原話：「但是你顯示 PGM 就很奇怪？」）。

擋這件事的守門需要知道「哪些字元是代號」。而那份名單**不可以寫死**：

  * 寫死方案 id（`t9_pinyin`）→ 市集裡任何第三方九宮格方案都不受保護；
  * 寫死 `ADGJMPTW` 八個字母 → 方案換一組代表字母，守門就靜靜地失效，
    而失效的樣子與全綠一模一樣。

所以這裡與 Kotlin 那一側走**同一條判準**：
**送出這個字元的那顆鍵，鍵面（label）不只它自己。**

⚠ 對照的實作是 android/app/src/main/java/org/luminakey/ime/keyboard/
   CandidateBarModel.kt 的 `InlinePreedit.groupCodeChars()`。兩邊必須同步 ——
   判準改了而這裡沒改，守門會開始放行（或誤報），兩種都不會有人叫。

用法
─────────────────────────────────────────────────────────────────────────────
    group_codes.py --layout core/layouts/cn-t9-pinyin.yaml
        印出這一層的代號字元，例如 ADGJMPTW

    group_codes.py --layout ... --run "MG GAM"
        印出文字裡**第一段連續兩個以上**的代號；沒有就什麼都不印。
        退出碼一律 0 —— 「有沒有違規」由有沒有輸出決定，
        呼叫端才能把「工具壞了」（非 0）與「驗到了違規」分開。
"""

from __future__ import annotations

import argparse
import sys

try:
    import yaml
except ImportError:
    sys.stderr.write("需要 PyYAML:pip3 install pyyaml\n")
    sys.exit(2)


def group_codes(layout_path, layer_id=None):
    """與 InlinePreedit.groupCodeChars 同一條判準。找不到層就丟 SystemExit(2)。"""
    with open(layout_path, encoding="utf-8") as f:
        doc = yaml.safe_load(f) or {}
    layers = doc.get("layers") or []
    if not layers:
        sys.exit("！ %s 沒有 layers" % layout_path)
    if layer_id is None:
        # §9.1.1 的初始層。九宮格佈局的第一層就是九宮格那一層。
        layer = layers[0]
    else:
        picked = [l for l in layers if l.get("id") == layer_id]
        if not picked:
            sys.exit("！ %s 裡沒有 %s 這一層" % (layout_path, layer_id))
        layer = picked[0]

    out = set()
    for row in layer.get("rows") or []:
        for key in row.get("keys") or []:
            if not isinstance(key, dict) or key.get("spacer"):
                continue
            send = key.get("send")
            if not isinstance(send, dict):
                continue
            keysym = send.get("keysym")
            if not isinstance(keysym, str) or len(keysym) != 1:
                continue
            label = key.get("label") or ""
            # 鍵面就是它自己（`M` → `M`）= 一鍵一字母，不是代號。
            if len(label) > 1:
                out.add(keysym)
    return out


def first_run(text, codes, least=2):
    """文字裡第一段長度 >= least 的連續代號；沒有回 ''。"""
    run = ""
    for ch in text:
        if ch in codes:
            run += ch
        else:
            if len(run) >= least:
                return run
            run = ""
    return run if len(run) >= least else ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--layout", required=True, help="佈局 YAML 的路徑")
    ap.add_argument("--layer", default=None, help="層 id,預設取第一層")
    ap.add_argument("--run", default=None, help="檢查這段文字裡有沒有連續代號")
    ap.add_argument("--least", type=int, default=2, help="連續幾個才算違規,預設 2")
    args = ap.parse_args()

    codes = group_codes(args.layout, args.layer)
    if args.run is None:
        sys.stdout.write("".join(sorted(codes)))
        return
    # 一個代號都抽不出來時**不可以**靜靜地回報「沒有違規」——
    # 那正是判準壞掉的樣子，而它看起來與通過一模一樣。
    if not codes:
        sys.exit("！ %s 抽不出任何按鍵代號 —— 判準壞了,守門會永遠是綠的" % args.layout)
    sys.stdout.write(first_run(args.run, codes, args.least))


if __name__ == "__main__":
    main()
