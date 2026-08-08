#!/usr/bin/env python3
"""RimeQuad 下載頁 —— 區網內開一個頁面,把三端最新版列出來。

為什麼要這個:發布地址散在對話紀錄裡找不回來,而且分不出哪個是新的。

為什麼每次請求都去讀 R2,而不是產生一份靜態 HTML:
  Android 是從 GitHub Actions 直接發到 R2 的,那條路徑碰不到這台機器。
  靜態頁一定會過期,而**過期的下載頁比沒有下載頁更糟** —— 使用者會拿到舊版,
  然後回報一個早就修好的 bug。所以這頁的真相來源只有一個:R2 本身。

用法:
  python3 scripts/downloads_server.py [--port 20000] [--bind 0.0.0.0]
"""

import argparse
import html
import json
import re
import subprocess
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REMOTE = "r2:tgapk/rime"
PUBLIC = "https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev/rime"
CACHE_SECONDS = 30

_cache = {"at": 0.0, "data": None}


def _rclone_list():
    out = subprocess.run(
        ["rclone", "lsjson", "-R", REMOTE, "--s3-no-check-bucket", "--files-only"],
        capture_output=True, text=True, timeout=60,
    )
    if out.returncode != 0:
        raise RuntimeError(out.stderr.strip()[:400] or "rclone 失敗")
    return json.loads(out.stdout)


def _android_version():
    """version.json 由 CI 的發布步驟寫,是 Android 的權威版本資訊。"""
    out = subprocess.run(
        ["rclone", "cat", f"{REMOTE}/version.json", "--s3-no-check-bucket"],
        capture_output=True, text=True, timeout=30,
    )
    if out.returncode != 0:
        return None
    try:
        return json.loads(out.stdout)
    except json.JSONDecodeError:
        return None


# 檔名裡的 <日期>-<時間>-<commit>。桌面兩端沒有 version.json,版本就在檔名上。
STAMP = re.compile(r"(\d{8})-(\d{4})-([0-9a-f]{7,40})")


def _stamp_of(name):
    m = STAMP.search(name)
    if not m:
        return None
    d, t, c = m.groups()
    return {
        "date": f"{d[:4]}-{d[4:6]}-{d[6:]}",
        "time": f"{t[:2]}:{t[2:]}",
        "commit": c,
        "sort": d + t,
    }


def _pick(files, prefix, suffix):
    """挑出某一端所有帶版本戳的檔案,最新的排前面。

    刻意跳過 *-latest.*:那是指標不是版本,列出來只會讓人分不清哪個是哪個。
    """
    got = []
    for f in files:
        p = f["Path"]
        if not (p.startswith(prefix) and p.endswith(suffix)):
            continue
        if "latest" in p:
            continue
        st = _stamp_of(p)
        if not st:
            continue
        got.append({"path": p, "size": f["Size"], **st})
    got.sort(key=lambda x: x["sort"], reverse=True)
    return got


def snapshot():
    now = time.time()
    if _cache["data"] and now - _cache["at"] < CACHE_SECONDS:
        return _cache["data"]
    files = _rclone_list()
    data = {
        "android": _pick(files, "rime-android-", ".apk"),
        "macos": _pick(files, "macos/RimeQuad-macos-", ".tar.gz"),
        "windows": _pick(files, "windows/RimeQuad-windows-", ".zip"),
        "android_version": _android_version(),
        "at": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    _cache.update(at=now, data=data)
    return data


def mb(n):
    return f"{n / 1048576:.1f} MB"


CSS = """
*{box-sizing:border-box}
body{margin:0;padding:32px 20px 64px;font:16px/1.6 -apple-system,BlinkMacSystemFont,"Segoe UI",
 "Noto Sans TC","PingFang TC","Microsoft JhengHei",sans-serif;
 background:#f6f7f9;color:#16181d}
.wrap{max-width:880px;margin:0 auto}
h1{font-size:26px;margin:0 0 4px}
.sub{color:#6b7280;font-size:14px;margin-bottom:28px}
.card{background:#fff;border:1px solid #e5e7eb;border-radius:14px;padding:20px 22px;margin-bottom:18px}
.head{display:flex;align-items:baseline;gap:10px;flex-wrap:wrap;margin-bottom:14px}
.name{font-size:20px;font-weight:600}
.tag{font-size:12px;padding:2px 9px;border-radius:999px;background:#eef2ff;color:#4338ca}
.tag.warn{background:#fef3c7;color:#92400e}
.dl{display:inline-block;background:#2563eb;color:#fff;text-decoration:none;
 padding:11px 20px;border-radius:9px;font-weight:600}
.dl:hover{background:#1d4ed8}
.meta{color:#6b7280;font-size:13px;margin-top:10px;font-variant-numeric:tabular-nums}
.meta code{background:#f3f4f6;padding:1px 5px;border-radius:4px;font-size:12px}
pre{background:#0f1115;color:#e5e7eb;padding:13px 15px;border-radius:9px;overflow-x:auto;
 font-size:12.5px;line-height:1.55;margin:12px 0 0}
details{margin-top:14px}
summary{cursor:pointer;color:#6b7280;font-size:13px}
.hist{font-size:13px;color:#6b7280;margin-top:8px}
.hist a{color:#6b7280}
.note{font-size:13.5px;color:#92400e;background:#fffbeb;border:1px solid #fde68a;
 border-radius:9px;padding:10px 13px;margin-top:12px}
.foot{color:#9ca3af;font-size:12.5px;text-align:center;margin-top:32px}
@media (prefers-color-scheme:dark){
 body{background:#0d0f13;color:#e7e9ee}
 .card{background:#161a20;border-color:#252b34}
 .sub,.meta,.hist,.hist a,summary{color:#9aa3af}
 .meta code{background:#252b34}
 .tag{background:#1e2a52;color:#a5b4fc}
 .tag.warn{background:#3a2f12;color:#fcd34d}
 .note{background:#2a2410;border-color:#4d3f14;color:#fcd34d}
}
"""

INSTALL = {
    "android": None,
    "macos": """cd ~/Downloads
tar xzf {file}
xattr -dr com.apple.quarantine RimeQuad.app
mkdir -p ~/Library/Input\\ Methods
mv RimeQuad.app ~/Library/Input\\ Methods/
open ~/Library/Input\\ Methods/RimeQuad.app""",
    "windows": None,
}


def card(title, tag, tag_cls, items, extra_meta="", note="", install=""):
    if not items:
        return (f'<div class="card"><div class="head"><span class="name">{title}</span>'
                f'<span class="tag warn">還沒有發布</span></div></div>')
    top = items[0]
    url = f"{PUBLIC}/{top['path']}"
    fname = top["path"].split("/")[-1]
    body = [
        '<div class="card">',
        '<div class="head">',
        f'<span class="name">{html.escape(title)}</span>',
        f'<span class="tag {tag_cls}">{html.escape(tag)}</span>',
        "</div>",
        f'<a class="dl" href="{html.escape(url)}">下載</a>',
        f'<div class="meta">{html.escape(fname)} · {mb(top["size"])} · '
        f'{top["date"]} {top["time"]} · <code>{top["commit"][:7]}</code>{extra_meta}</div>',
    ]
    if note:
        body.append(f'<div class="note">{note}</div>')
    if install:
        body.append(f"<pre>{html.escape(install.format(file=fname))}</pre>")
    if len(items) > 1:
        old = "".join(
            f'<div><a href="{html.escape(PUBLIC + "/" + i["path"])}">'
            f'{html.escape(i["path"].split("/")[-1])}</a> · {mb(i["size"])}</div>'
            for i in items[1:8]
        )
        body.append(f'<details><summary>舊版 ({len(items) - 1})</summary>'
                    f'<div class="hist">{old}</div></details>')
    body.append("</div>")
    return "".join(body)


def page():
    try:
        d = snapshot()
    except Exception as e:                                    # noqa: BLE001
        return (f'<div class="wrap"><h1>讀不到 R2</h1><pre>{html.escape(str(e))}</pre>'
                "<p>rclone 的 r2 remote 還在嗎?</p></div>")

    av = d["android_version"] or {}
    a_meta = f' · versionCode <code>{av["version_code"]}</code>' if av.get("version_code") else ""

    parts = [
        '<div class="wrap">',
        "<h1>RimeQuad 下載</h1>",
        f'<div class="sub">四端 RIME 輸入法 · 清單直接讀自 R2,{d["at"]} 更新</div>',
        card("Android", "可用的產品", "", d["android"], extra_meta=a_meta),
        card("macOS (Apple Silicon)", "第一次可安裝", "warn", d["macos"],
             note="還沒有安裝程式,要照下面的步驟裝。"
                  "<b>一定要放進 ~/Library/Input Methods</b> —— 放到 /Applications "
                  "會靜默失敗:系統照樣登錄它,但永遠不會出現在輸入來源清單裡。"
                  "裝完登出再登入。",
             install=INSTALL["macos"]),
        card("Windows x64", "第一次可安裝", "warn", d["windows"],
             note="解壓到一個固定位置(之後不要搬,註冊表記的是路徑),"
                  "雙擊 <code>install.bat</code>,它會自己要求管理員權限。"
                  "DLL 沒有簽章,而 TSF 的 DLL 會被載入到每一個接受文字輸入的程式裡。"),
        '<div class="foot">桌面兩端目前沒有設定介面,只能用預設方案。iOS 還沒開始。</div>',
        "</div>",
    ]
    return "".join(parts)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):                                          # noqa: N802
        if self.path not in ("/", "/index.html"):
            self.send_error(404)
            return
        body = (f"<!doctype html><html lang=zh-Hant><head><meta charset=utf-8>"
                f'<meta name=viewport content="width=device-width,initial-scale=1">'
                f"<title>RimeQuad 下載</title><style>{CSS}</style></head>"
                f"<body>{page()}</body></html>").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=20000)
    ap.add_argument("--bind", default="0.0.0.0")
    ns = ap.parse_args()
    print(f"下載頁: http://{ns.bind}:{ns.port}/")
    ThreadingHTTPServer((ns.bind, ns.port), Handler).serve_forever()
