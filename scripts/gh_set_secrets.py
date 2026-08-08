#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gh_set_secrets.py — 把發布所需的機密上傳到 GitHub Actions Secrets。

為什麼要有這一支(而不是在網頁上手動貼):
  · 這些機密目前只存在於一台 Ubuntu 上。那台機器掛掉,專案就發不了版。
    有這支腳本,任何一台拿得到 ~/rime-signing 備份的機器都能在三分鐘內
    把發布能力重建回來。
  · GitHub 的 secrets API **不收明文**。它要求先取得 repo 的公鑰,再用
    libsodium 的 sealed box(crypto_box_seal:X25519 + XSalsa20-Poly1305,
    nonce = blake2b(ephemeral_pk || recipient_pk))加密,base64 之後 PUT。
    手打很容易做錯,而做錯的徵狀是 CI 拿到一串亂碼 —— 那會在建置到一半
    才炸,訊息還看不出原因。
  · **這支腳本永遠不印任何機密的值。** 只印名稱、長度級距與 HTTP 狀態。

用法:
    python3 scripts/gh_set_secrets.py                 # 全部上傳
    python3 scripts/gh_set_secrets.py --dry-run       # 只檢查來源檔齊不齊
    python3 scripts/gh_set_secrets.py --only ANDROID_KEYSTORE_BASE64 ...

來源:
    $RIME_SIGNING_DIR(預設 ~/rime-signing)
        signing.properties / rime-release.jks / signing-lineage.bin / old-debug.keystore
    $RCLONE_CONFIG(預設 ~/.config/rclone/rclone.conf)的 [r2] 段
    $GITHUB_TOKEN_FILE(預設 ~/.github-token 或 /home/lc/.github-token)
"""

import argparse
import base64
import ctypes
import ctypes.util
import json
import os
import sys
import urllib.error
import urllib.request

DEFAULT_REPO = "IncredibleGG/rime-quad"


def die(msg):
    print("錯誤: %s" % msg, file=sys.stderr)
    sys.exit(1)


# --------------------------------------------------------------- sealed box ---
def _seal_pynacl(message: bytes, public_key: bytes):
    try:
        from nacl.public import PublicKey, SealedBox  # type: ignore
    except Exception:
        return None
    return bytes(SealedBox(PublicKey(public_key)).encrypt(message))


def _seal_libsodium(message: bytes, public_key: bytes):
    """用系統的 libsodium 做 crypto_box_seal。

    這台機器沒有 PyNaCl 也沒有 sudo,但 /usr/lib 下有 libsodium.so.23。
    ctypes 直接叫就好 —— 這比自己用 Python 重寫 X25519 + XSalsa20-Poly1305
    安全得多(自己寫的密碼學出錯不會有任何徵狀)。
    """
    for cand in (ctypes.util.find_library("sodium"), "libsodium.so.23",
                 "libsodium.so", "libsodium.dylib"):
        if not cand:
            continue
        try:
            lib = ctypes.CDLL(cand)
            break
        except OSError:
            continue
    else:
        return None
    if lib.sodium_init() < 0:
        return None
    sealbytes = lib.crypto_box_sealbytes()          # 48
    if lib.crypto_box_publickeybytes() != len(public_key):
        die("repo 公鑰長度不對(%d),不是 X25519 公鑰" % len(public_key))
    out = ctypes.create_string_buffer(len(message) + sealbytes)
    rc = lib.crypto_box_seal(out, message, ctypes.c_ulonglong(len(message)), public_key)
    if rc != 0:
        die("crypto_box_seal 失敗")
    return out.raw[:len(message) + sealbytes]


def seal(message: bytes, public_key_b64: str) -> str:
    pk = base64.b64decode(public_key_b64)
    box = _seal_pynacl(message, pk) or _seal_libsodium(message, pk)
    if box is None:
        die("找不到 libsodium,也沒有 PyNaCl。GitHub 的 secrets API 只收 "
            "sealed box 密文,無法以明文 PUT。\n"
            "  Debian/Ubuntu: apt install libsodium23  或  pip install --user pynacl")
    # 自我檢查:sealed box 一定比明文多 48 bytes。差一個 byte 都代表接錯函式。
    if len(box) != len(message) + 48:
        die("sealed box 長度異常(%d vs %d+48),拒絕上傳" % (len(box), len(message)))
    return base64.b64encode(box).decode("ascii")


# ------------------------------------------------------------------ 來源讀取 ---
def read_props(path):
    props = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            props[k.strip()] = v.strip()
    return props


def read_rclone_section(path, section="r2"):
    """抽出 rclone.conf 裡的某一段,連同段標題一起回傳。

    只取這一段:那個檔案裡可能還有別的專案的憑證,整份上傳等於把不相干的
    存取權也交出去。
    """
    out, inside = [], False
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            s = line.strip()
            if s.startswith("[") and s.endswith("]"):
                inside = (s[1:-1].strip() == section)
                if inside:
                    out.append("[%s]\n" % section)
                continue
            if inside and s:
                out.append(line if line.endswith("\n") else line + "\n")
    if not out:
        die("在 %s 裡找不到 [%s] 段" % (path, section))
    return "".join(out)


def find_token():
    p = os.environ.get("GITHUB_TOKEN_FILE")
    cands = [p] if p else []
    cands += [os.path.expanduser("~/.github-token"), "/home/lc/.github-token"]
    for c in cands:
        if c and os.path.isfile(c):
            with open(c, "r", encoding="utf-8") as f:
                return f.read().strip()
    if os.environ.get("GITHUB_TOKEN"):
        return os.environ["GITHUB_TOKEN"].strip()
    die("找不到 GitHub token(~/.github-token 或 $GITHUB_TOKEN)")


# ---------------------------------------------------------------- GitHub API ---
def api(token, method, url, payload=None):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", "token " + token)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("X-GitHub-Api-Version", "2022-11-28")
    req.add_header("User-Agent", "rime-quad-secrets")
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            body = r.read()
            return r.status, (json.loads(body) if body else None)
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")
        # 這裡刻意只印狀態碼與 GitHub 的 message,不回顯我們送出去的 payload。
        return e.code, {"raw": body[:300]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=os.environ.get("RIME_GH_REPO", DEFAULT_REPO))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--only", nargs="*", default=None)
    args = ap.parse_args()

    sdir = os.environ.get("RIME_SIGNING_DIR", os.path.expanduser("~/rime-signing"))
    rconf = os.environ.get("RCLONE_CONFIG", os.path.expanduser("~/.config/rclone/rclone.conf"))

    for f in ("signing.properties", "rime-release.jks", "signing-lineage.bin",
              "old-debug.keystore"):
        if not os.path.isfile(os.path.join(sdir, f)):
            die("缺少 %s/%s —— 沒有它 CI 產不出能升級的 APK" % (sdir, f))
    if not os.path.isfile(rconf):
        die("找不到 rclone 設定 %s" % rconf)

    props = read_props(os.path.join(sdir, "signing.properties"))

    def need(key):
        v = props.get(key)
        if not v:
            die("signing.properties 缺少 %s" % key)
        return v

    def b64file(name):
        with open(os.path.join(sdir, name), "rb") as f:
            return base64.b64encode(f.read()).decode("ascii")

    # 名稱與 .github/workflows/build.yml 的「重建簽章環境」步驟一一對應。
    # 改名字要兩邊一起改,否則 CI 會安靜地退回 debug 金鑰。
    secrets = {
        "ANDROID_KEYSTORE_BASE64":        b64file("rime-release.jks"),
        "ANDROID_KEYSTORE_PASSWORD":      need("storePassword"),
        "ANDROID_KEY_ALIAS":              need("keyAlias"),
        "ANDROID_KEY_PASSWORD":           need("keyPassword"),
        "ANDROID_LINEAGE_BASE64":         b64file("signing-lineage.bin"),
        "ANDROID_OLD_KEYSTORE_BASE64":    b64file("old-debug.keystore"),
        "ANDROID_OLD_KEY_ALIAS":          need("oldKeyAlias"),
        # 舊 debug keystore 的密碼**刻意不放進來**。那是 Android 工具寫死的
        # 公開常數 "android",不是秘密;而把它設成 secret 有實際害處 ——
        # GitHub 會遮罩它,於是日誌裡每一個 "android" 都變成 ***
        # (`/usr/local/lib/***/sdk`、`***/app/build/...`),排查時什麼都看不出來。
        # 值寫在 .github/actions/restore-signing/action.yml 的 default 裡。
        "R2_RCLONE_CONF_BASE64":
            base64.b64encode(read_rclone_section(rconf, "r2").encode()).decode("ascii"),
    }

    if args.only:
        unknown = [k for k in args.only if k not in secrets]
        if unknown:
            die("不認得的 secret 名稱: %s" % ", ".join(unknown))
        secrets = {k: v for k, v in secrets.items() if k in args.only}

    print("repo: %s" % args.repo)
    print("來源: %s、%s" % (sdir, rconf))
    print("待上傳(只列名稱,值不會出現在任何輸出裡):")
    for k, v in secrets.items():
        print("  · %-30s (%d bytes)" % (k, len(v)))

    if args.dry_run:
        print("\n--dry-run:沒有連線,沒有上傳。")
        return

    token = find_token()
    base = "https://api.github.com/repos/%s/actions/secrets" % args.repo
    st, key = api(token, "GET", base + "/public-key")
    if st != 200 or not key:
        die("取 repo 公鑰失敗(HTTP %s): %s" % (st, key))
    key_id, key_b64 = key["key_id"], key["key"]

    print("\n上傳中(sealed box, key_id=%s):" % key_id)
    bad = 0
    for name, value in secrets.items():
        st, resp = api(token, "PUT", "%s/%s" % (base, name), {
            "encrypted_value": seal(value.encode("utf-8"), key_b64),
            "key_id": key_id,
        })
        if st in (201, 204):
            print("  [OK]   %-30s HTTP %s" % (name, st))
        else:
            print("  [失敗] %-30s HTTP %s %s" % (name, st, resp), file=sys.stderr)
            bad += 1

    st, listing = api(token, "GET", base + "?per_page=100")
    if st == 200:
        got = {s["name"] for s in listing.get("secrets", [])}
        missing = [n for n in secrets if n not in got]
        if missing:
            print("  [失敗] 上傳後仍看不到: %s" % ", ".join(missing), file=sys.stderr)
            bad += 1
        else:
            print("\n已確認 repo 上存在這 %d 個 secret(共 %d 個)。"
                  % (len(secrets), len(got)))
    if bad:
        sys.exit(1)


if __name__ == "__main__":
    main()
