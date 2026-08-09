#!/usr/bin/env python3
"""下載頁 —— 區網內開一個頁面,把三端最新版列出來。

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
import os
import re
import subprocess
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# 產品名與 R2 上的檔名都從共用設定來,見 scripts/lib/product.env。
# ⚠ 產品名改了,R2 的路徑與檔名**沒有**跟著改 —— 頁面上寫的是新名字,
#   下載下來的檔案還叫舊名字。那不是漏改:應用內升級與已經發出去的連結
#   都指著那些名字,改了會斷。等發布搬到 GitHub Releases 時一起換。
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "lib"))
import product  # noqa: E402

REMOTE = product.R2_REMOTE
PUBLIC = f"https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev/{product.R2_PREFIX}"
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

    prefix 可以是字串或多個字串。多個時同時認 —— 改名之後 R2 上新舊檔名
    並存,只認新的會讓「舊版」欄一夕之間變空,看起來像資料掉了。

    刻意跳過 *-latest.*:那是指標不是版本,列出來只會讓人分不清哪個是哪個。
    """
    prefixes = (prefix,) if isinstance(prefix, str) else tuple(prefix)
    got = []
    for f in files:
        p = f["Path"]
        if not (any(p.startswith(x) for x in prefixes) and p.endswith(suffix)):
            continue
        if "latest" in p:
            continue
        st = _stamp_of(p)
        if not st:
            continue
        got.append({"path": p, "size": f["Size"], **st})
    got.sort(key=lambda x: x["sort"], reverse=True)
    return got


# R2 上的目錄與檔名字根(刻意保留舊名,見檔頭)。
_MAC = product.R2_MACOS_DIR
_WIN = product.R2_WINDOWS_DIR
_BASE = product.R2_ARTIFACT_BASE
# 新字根在前、舊的在後:改名之後 R2 上兩種檔名並存,只認新的會讓「舊版」
# 欄一夕之間變空,看起來像資料掉了。
_BASES = [_BASE, getattr(product, "R2_ARTIFACT_BASE_LEGACY", "")]
_BASES = [b for b in _BASES if b]


def snapshot():
    now = time.time()
    if _cache["data"] and now - _cache["at"] < CACHE_SECONDS:
        return _cache["data"]
    files = _rclone_list()
    data = {
        "android": _pick(files, product.R2_ANDROID_APK_PREFIX, ".apk"),
        # 安裝程式是主要下載;壓縮包留給想手動放的人。
        "macos": _pick(files, [f"{_MAC}/{b}-macos-" for b in _BASES], ".pkg"),
        "macos_archive": _pick(files, [f"{_MAC}/{b}-macos-" for b in _BASES], ".tar.gz"),
        "windows": _pick(files, [f"{_WIN}/{b}-Setup-x64-" for b in _BASES], ".exe"),
        "windows_archive": _pick(files, [f"{_WIN}/{b}-windows-" for b in _BASES], ".zip"),
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

# R2 上的這個小檔案記著「最新那一份 macOS 壓縮包裡的 .app 到底叫什麼」。
# 由 scripts/publish_desktop.sh 在發布時**從產物本身量出來**之後寫上去。
MACOS_BUNDLE_KEY = f"{product.R2_MACOS_DIR}/app-bundle-latest.txt"

_APP_NAME_RE = re.compile(r"[A-Za-z0-9._-]{1,64}\.app")

_bundle_cache = {"at": 0.0, "name": None}


def macos_app_bundle():
    """手動安裝指令裡那個 `.app` 叫什麼。

    ⚠ **不可以從產品名推。** R2 上躺著的是**上一次發布**的產物,而「產品改名」
    與「改名後發布過一版」之間必然有一段時間差 —— 那段時間裡從產品名推出來的
    名字是錯的。名字錯了的症狀是:使用者照著四步做完,系統一聲不吭地不載入它,
    沒有任何錯誤訊息(見 scripts/lib/product.env 的 MACOS_APP_BUNDLE)。

    所以真相來源與這一頁的其他東西一樣,只有 R2 本身;讀不到那個檔案時才退回
    product.env 的值 —— 那代表 R2 上還是改名前發布的那一份,而那個常數記的
    正好就是它。
    """
    now = time.time()
    if _bundle_cache["name"] and now - _bundle_cache["at"] < CACHE_SECONDS:
        return _bundle_cache["name"]
    name = None
    try:
        out = subprocess.run(
            ["rclone", "cat", f"{REMOTE}/{MACOS_BUNDLE_KEY}", "--s3-no-check-bucket"],
            capture_output=True, text=True, timeout=30,
        )
        if out.returncode == 0:
            got = out.stdout.strip()
            if _APP_NAME_RE.fullmatch(got):
                name = got
    except (OSError, subprocess.SubprocessError):
        name = None
    name = name or product.MACOS_APP_BUNDLE
    _bundle_cache.update(at=now, name=name)
    return name


def macos_install_text(app_bundle, archive_name):
    """手動安裝的四步。**publish_desktop.sh 會拿這段字去和產物比對。**

    四步裡任何一步的名字錯了都是靜默失敗,所以這段字只有一個產生點。
    """
    return (
        "cd ~/Downloads\n"
        f"tar xzf {archive_name}\n"
        f"xattr -dr com.apple.quarantine {app_bundle}\n"
        "mkdir -p ~/Library/Input\\ Methods\n"
        f"mv {app_bundle} ~/Library/Input\\ Methods/\n"
        f"open ~/Library/Input\\ Methods/{app_bundle}"
    )


def card(title, tag, tag_cls, items, extra_meta="", note="", install="", alt=None):
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
        body.append(f"<pre>{html.escape(install)}</pre>")
    if alt and alt[1]:
        a = alt[1][0]
        body.append(
            f'<div class="meta" style="margin-top:14px">{html.escape(alt[0])}:'
            f'<a href="{html.escape(PUBLIC + "/" + a["path"])}">'
            f'{html.escape(a["path"].split("/")[-1])}</a> · {mb(a["size"])}</div>'
        )
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
        f"<h1>{html.escape(product.PRODUCT_NAME)} 下載</h1>",
        f'<div class="sub">{html.escape(product.PRODUCT_NAME_ZH)} · 以 RIME(librime)為引擎 · '
        f'清單直接讀自 R2,{d["at"]} 更新</div>',
        card("Android", "可用的產品", "", d["android"], extra_meta=a_meta,
             # ⚠ 這一段不是客套話,是一則真的會咬人的事:產品改名時 applicationId
             #   從 org.rimequad.ime 換成 org.luminakey.ime,而 Android 的
             #   PackageInstaller 只認套件名 —— 舊 app 裡的「檢查更新」抓到新版、
             #   下載 30MB、然後一定會失敗,而錯誤訊息說的是「檔案無效或已損毀」,
             #   看起來像我們的檔案壞了。舊 app 已經發出去了,改不動它,
             #   所以唯一能講這句話的地方就是這裡。
             note=f"<b>如果你手機上裝的是改名前的版本</b>(圖示一樣、但那時叫 RimeQuad),"
                  f"app 內的「檢查更新」會失敗,顯示「APK 檔案無效或已損毀」—— "
                  f"那不是檔案壞了,是 Android 不允許換套件名的升級。"
                  f"請先<b>解除安裝舊的那一個</b>,再裝下面這份。"
                  f"⚠ 解除安裝會一併刪掉舊 app 裡的詞庫與設定,"
                  f"想留的話先在舊 app 裡「匯出詞庫」。"),
        card("macOS (Apple Silicon)", "帶設定介面", "warn", d["macos"],
             note="雙擊 .pkg,下一步到底,<b>裝完登出再登入</b>。"
                  f"然後系統設定 → 鍵盤 → 輸入來源 → + → 繁體中文/簡體中文 → {product.PRODUCT_NAME}。"
                  "設定從選單列上的輸入法圖示打開。"
                  "ad-hoc 簽章,不是可散布的正式版本。",
             install=(macos_install_text(macos_app_bundle(),
                                         d["macos_archive"][0]["path"].split("/")[-1])
                      if d["macos_archive"] else ""),
             alt=("手動安裝(進階)", d["macos_archive"])),
        card("Windows x64", "帶設定介面", "warn", d["windows"],
             note="雙擊 Setup.exe,它會自己要求管理員權限。"
                  f"裝完:設定 → 時間與語言 → 語言與地區 → 中文 → 選項 → 新增鍵盤 → {product.PRODUCT_NAME}。"
                  "設定從語言列按鈕或系統匣圖示打開。"
                  "<b>沒有程式碼簽章</b>,SmartScreen 會攔;而 TSF 的 DLL 會被載入到"
                  "每一個接受文字輸入的程式裡。",
             alt=("手動安裝(進階)", d["windows_archive"])),
        '<div class="foot">桌面兩端的設定介面是第一版,沒有人按過每一顆按鈕。iOS 還沒開始。</div>',
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
                f"<title>{product.PRODUCT_NAME} 下載</title><style>{CSS}</style></head>"
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
    # 給 scripts/publish_desktop.sh 用:印出這一頁**現在會叫使用者打的那幾行**。
    # 發布時拿它和壓縮包裡真的那個 .app 比對,對不上就不准發。
    ap.add_argument("--print-macos-install", action="store_true",
                    help="印出 macOS 手動安裝指令(發布關卡用),不起伺服器")
    ns = ap.parse_args()
    if ns.print_macos_install:
        print(macos_install_text(macos_app_bundle(), "<壓縮包>"))
        raise SystemExit(0)
    print(f"下載頁: http://{ns.bind}:{ns.port}/")
    ThreadingHTTPServer((ns.bind, ns.port), Handler).serve_forever()
