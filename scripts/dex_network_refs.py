#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dex_network_refs.py — 在**最終產物**上驗「單一連網出口」

═══════════════════════════════════════════════════════════════════════════
 為什麼要在 APK 上驗，而不是 grep 原始碼
═══════════════════════════════════════════════════════════════════════════

audit_offline.sh 第 1 項是對 `android/app/src` 下 grep。那擋得住**我們自己**
多寫一個出口，擋不住兩件事：

  1. **傳遞相依。** build.gradle.kts 只寫了 androidx 那幾行，但 androidx 自己
     會拉進別的東西。實測這個 APK 裡就有 okio（androidx.datastore 帶進來的），
     而 okio 有 `Okio.source(Socket)` 這類 socket 輔助函式。原始碼 grep 一個字
     都看不到它。
  2. **改名／反射／別的語言。** dex 裡存的是型別描述子，改名改不掉。

所以這支腳本直接問 APK：`java.net.HttpURLConnection` 這個型別，**整個 dex 裡
有哪些類別引用它**？答案必須正好是 `org.rimequad.ime.net.NetworkGate`。

═══════════════════════════════════════════════════════════════════════════
 判準是**集合相等**，不是「沒有壞東西」
═══════════════════════════════════════════════════════════════════════════

多一個引用者 → 紅（有人開了第二條路，或某個相依帶進來一個）。
少一個引用者 → **也紅**。少掉不是「更安全」：NetworkGate 不再引用
HttpURLConnection，通常代表連網搬去別的地方了，而那正是最該被發現的一種變化。

清單變動時要有人**明確**改這個檔案並在 commit message 裡說明為什麼——
這就是重點。升級 androidx 讓某個工具類消失是正常的；正常的事也要有人看過。

用法：
    scripts/dex_network_refs.py <apk>
    scripts/dex_network_refs.py <apk> --dexdump /path/to/dexdump
    scripts/dex_network_refs.py <apk> --extra-referrer 'Lcom/evil/X;:Ljava/net/Socket;'
        ↑ 反向測試用：假裝 dex 裡多了一個引用者，這支腳本必須因此紅。
          （由 scripts/verify_audit_offline.sh 使用）

離開碼：0 = 完全相符；1 = 有出入；2 = 跑不起來（工具不在、APK 讀不到）。
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

# ── 釘死的引用者表 ─────────────────────────────────────────────────────────
# 2026-08-08 在 app-debug.apk（未經 R8 縮減，所以是**最寬**的一份）上量出來的。
# release 建置經過 R8 之後只會更少，而「更少」在這裡同樣要紅、同樣要有人看過。
EXPECTED = {
    # ★ 這一條是「單一連網出口」在產物層的證據。整個 APK 只有一個類別
    #   引用得到 HttpURLConnection —— 就是那個檔頭寫著「請自己 grep」的檔案。
    "Ljava/net/HttpURLConnection;": {
        "Lorg/rimequad/ime/net/NetworkGate;",
    },
    "Ljava/net/URLConnection;": {
        "Lorg/rimequad/ime/net/NetworkGate;",
    },
    # java.net.URL 本身連不了線（要 openConnection，見上面兩條）。這三個
    # 非我們的引用者都不是在連網：kotlin 的 URL.readText 擴充、coroutines 的
    # ServiceLoader（讀 classpath 資源）、okio 讀 jar: 內的資源。
    "Ljava/net/URL;": {
        "Lorg/rimequad/ime/net/NetworkGate;",
        "Lkotlin/io/TextStreamsKt;",
        "Lkotlinx/coroutines/internal/FastServiceLoader;",
        "Lokio/internal/ResourceFileSystem$Companion;",
    },
    # androidx.core 的流量統計工具類與 okio 的 socket 輔助。
    # 兩者都只是**能力**，我們的程式碼一行都沒有呼叫它們。
    # okio 是 androidx.datastore 帶進來的（見 :app:dependencies）。
    "Ljava/net/Socket;": {
        "Landroidx/core/net/DatagramSocketWrapper;",
        "Landroidx/core/net/TrafficStatsCompat;",
        "Lokio/-DeprecatedOkio;",
        "Lokio/Okio;",
        "Lokio/Okio__JvmOkioKt;",
        "Lokio/SocketAsyncTimeout;",
    },
    "Ljava/net/DatagramSocket": {
        "Landroidx/core/net/DatagramSocketWrapper$DatagramSocketImplWrapper;",
        "Landroidx/core/net/DatagramSocketWrapper;",
        "Landroidx/core/net/TrafficStatsCompat$Api24Impl;",
        "Landroidx/core/net/TrafficStatsCompat;",
    },
    # LinkifyCompat 為了拿 WebView 的 URL 正規表示式而引用它，不會建立 WebView。
    # 我們自己沒有任何 WebView（audit_offline.sh 第 3 項另外守著原始碼那一側）。
    "Landroid/webkit/WebView": {
        "Landroidx/core/text/util/LinkifyCompat;",
    },
    # 這三個必須是空的。
    "Ljava/net/ServerSocket": set(),
    "Ljava/net/MulticastSocket": set(),
    "Ljavax/net/ssl/SSLSocket": set(),
}


def find_dexdump():
    for root in (os.environ.get("ANDROID_SDK_ROOT"), os.environ.get("ANDROID_HOME"),
                 os.path.expanduser("~/Android/Sdk")):
        if not root:
            continue
        cands = sorted(glob.glob(os.path.join(root, "build-tools", "*", "dexdump")))
        if cands:
            return cands[-1]
    return shutil.which("dexdump")


def collect(apk, dexdump):
    tmp = tempfile.mkdtemp(prefix="rimequad-dexrefs.")
    try:
        with zipfile.ZipFile(apk) as z:
            names = [n for n in z.namelist() if re.fullmatch(r"classes\d*\.dex", n)]
            if not names:
                print("[error] APK 裡沒有 classes*.dex —— 掃描沒有真的執行", file=sys.stderr)
                return None
            for n in names:
                z.extract(n, tmp)
        found = {tok: set() for tok in EXPECTED}
        ndex = 0
        for d in sorted(glob.glob(os.path.join(tmp, "classes*.dex"))):
            ndex += 1
            out = subprocess.run([dexdump, "-d", d], capture_output=True,
                                 text=True, errors="replace").stdout
            if not out.strip():
                print("[error] dexdump 對 %s 沒有輸出" % os.path.basename(d), file=sys.stderr)
                return None
            cur = None
            for line in out.split("\n"):
                m = re.match(r"\s*Class descriptor\s*:\s*.(L[^;]+;)", line)
                if m:
                    cur = m.group(1)
                    continue
                if cur is None:
                    continue
                for tok in EXPECTED:
                    if tok in line:
                        # 引用者是自己的話不算（TrafficStatsCompat 引用自己之類）
                        found[tok].add(cur)
        # 把「自己引用自己」拿掉：型別描述子出現在自己的類別區塊裡是必然的。
        for tok in found:
            found[tok].discard(tok if tok.endswith(";") else tok + ";")
        return found, ndex
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("apk")
    ap.add_argument("--dexdump", default=None)
    ap.add_argument("--extra-referrer", action="append", default=[],
                    help="反向測試用：'<引用者>:<被引用型別>'")
    args = ap.parse_args()

    if not os.path.isfile(args.apk):
        print("[error] 找不到 APK：%s" % args.apk, file=sys.stderr)
        return 2
    dexdump = args.dexdump or find_dexdump()
    if not dexdump or not os.path.exists(dexdump):
        print("[error] 找不到 dexdump（Android build-tools 裡）", file=sys.stderr)
        return 2

    got = collect(args.apk, dexdump)
    if got is None:
        return 2
    found, ndex = got

    for spec in args.extra_referrer:
        ref, _, tok = spec.rpartition(":")
        if tok not in found:
            print("[error] --extra-referrer 的型別 %r 不在檢查清單裡" % tok, file=sys.stderr)
            return 2
        found[tok].add(ref)

    bad = False
    for tok in sorted(EXPECTED):
        extra = found[tok] - EXPECTED[tok]
        missing = EXPECTED[tok] - found[tok]
        if extra:
            bad = True
            print("  [FAIL] %s 多了 %d 個引用者：" % (tok, len(extra)))
            for c in sorted(extra):
                print("           + %s" % c)
        if missing:
            bad = True
            print("  [FAIL] %s 少了 %d 個引用者（少掉也要有人看過，見檔頭）：" % (tok, len(missing)))
            for c in sorted(missing):
                print("           - %s" % c)
    if bad:
        print("\n  引用者清單與 scripts/dex_network_refs.py 釘死的那份不一致。")
        print("  若這是預期中的變動（升級了某個相依），改那份清單並在 commit")
        print("  message 裡說明為什麼 —— 重點就是要有人明確看過一次。")
        return 1

    total = sum(len(v) for v in found.values())
    print("  [PASS] %d 個 dex，%d 個網路型別的引用者與釘死的清單完全相符"
          % (ndex, total))
    print("         其中 java.net.HttpURLConnection 的引用者只有 "
          "org.rimequad.ime.net.NetworkGate —— 單一出口在**產物**上成立")
    return 0


if __name__ == "__main__":
    sys.exit(main())
