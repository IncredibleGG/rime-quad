#!/usr/bin/env python3
"""product.py —— product.env 的 python 讀取端。

    import sys, pathlib
    sys.path.insert(0, str(pathlib.Path(__file__).parent / "lib"))
    import product
    print(product.ANDROID_IME_ID)

解析規則與 product.sh 逐字相同:切第一個 `=`,值原樣採用,不展開、不去引號。
推導值(IME id、套件路徑、NetworkGate 類別名 …)在兩邊各寫一次 ——
`scripts/verify_product_ids.sh` 每次都會把兩邊的輸出逐行比對,對不上就紅。

為什麼不讓 python 去跑 bash 讀:那樣 python 端在沒有 bash 的環境下就死了,
而且會把「設定檔壞了」變成「subprocess 回傳非零」。兩邊各十行,加一條比對,
比互相呼叫可靠。
"""

import os
import sys

_ENV_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "product.env")


def load(path=_ENV_PATH):
    """讀 product.env,回傳 {KEY: 值}。"""
    out = {}
    with open(path, encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                raise ValueError(f"{path}:{lineno} 這一行沒有 '='：{line!r}")
            key, val = line.split("=", 1)
            if not key or not all(c.isupper() or c.isdigit() or c == "_" for c in key):
                raise ValueError(f"{path}:{lineno} 不合法的鍵名：{key!r}")
            out[key] = val
    return out


def derive(v):
    """從原子事實推導組合值。**這一段必須與 product.sh 的推導一字不差。**"""
    d = dict(v)
    d["ANDROID_IME_ID"] = f"{v['ANDROID_APP_ID']}/{v['ANDROID_IME_SERVICE']}"
    d["ANDROID_PKG_PATH"] = v["ANDROID_APP_ID"].replace(".", "/")
    d["NETWORK_GATE_CLASS"] = f"{v['ANDROID_APP_ID']}.net.NetworkGate"
    d["NETWORK_GATE_REL"] = f"main/java/{d['ANDROID_PKG_PATH']}/net/NetworkGate.kt"
    d["R2_REMOTE"] = f"r2:tgapk/{v['R2_PREFIX']}"
    return d


VALUES = derive(load())

# 把每個鍵掛成模組屬性,`product.ANDROID_IME_ID` 就直接可用。
globals().update(VALUES)


if __name__ == "__main__":
    # --dump 的輸出格式與 `product.sh --dump` 相同(RS_ 前綴 + 依鍵名排序),
    # verify_product_ids.sh 拿兩份去 diff。
    if len(sys.argv) > 1 and sys.argv[1] == "--dump":
        for k in sorted(VALUES):
            print(f"RS_{k}={VALUES[k]}")
    else:
        sys.exit("用法: product.py --dump")
