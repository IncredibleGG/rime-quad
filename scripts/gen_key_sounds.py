#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
產生鍵盤的按鍵音素材。

── 為什麼是「產生」而不是「找一組音檔放進來」 ────────────────────────────
這個專案的每一份出貨資料都要說得出來源。第三方音效素材的授權要嘛得維護一份
attribution 檔、要嘛哪天被撤，兩種都會變成日後的負債；而「聽起來像鍵盤」的
音效其實只是幾條包絡線乘上幾個振盪器 —— 與其去找，不如把配方寫下來。

於是來源就是這一支腳本本身:任何人都能重跑它、比對 PCM 的 sha256,確認出貨的
音檔沒有夾帶別的東西。授權跟著本專案走(見 THIRD_PARTY_NOTICES.md 的說明)。

── 只用標準函式庫 ────────────────────────────────────────────────────────
⚠ **不要在這裡引進 numpy / scipy 或任何第三方套件。**

這支腳本被 scripts/verify_key_feedback.py 當成**守門**用(它 --check 一次,
確認出貨的 .ogg 真的是這份配方合出來的)。守門多一個相依,就多一個「在別人
的機器上跑不起來」的理由 —— 而守門跑不起來時,CI 上長得跟「有東西壞掉」
一模一樣,於是大家開始把它當雜訊。這條路我們走過:2026-08-12 那一輪
Android 車道就是紅在 `ModuleNotFoundError: No module named 'numpy'`,
產品一點事都沒有。

要做的事只有正弦、指數包絡、一階 IIR 低通與高斯雜訊,全部是 math 做得到的
純量運算,樣本數也只有幾千個。numpy 在這裡買到的是速度,而這支腳本一次跑完
不到一秒 —— 那不是值得用相依換的東西。

唯一的外部相依是 **ffmpeg**(編 .ogg / 解 .ogg),它本來就必須有:Python 的
標準函式庫沒有 Vorbis 編碼器。沒有它時腳本會直說,不會假裝通過。

── 決定性 ────────────────────────────────────────────────────────────────
每一個 (音色, 角色) 用一個固定的種子產生雜訊,所以**PCM 是逐位元可重現的**。
腳本會印出每一份 PCM 的 sha256。

亂數用 random.Random(seed).random(),這是 CPython 明文保證跨版本穩定的那一支
(Mersenne Twister,見 random 模組文件的相容性承諾);高斯雜訊用 Box–Muller
自己算,不用 random.gauss() —— 後者的實作沒有同一份承諾。

⚠ `.ogg` 的位元組**不**保證可重現 —— 那取決於 libvorbis 的版本。要驗的是
PCM 那一欄:`--check` 會重新合成一次並與現有 `.ogg` 解回來的 PCM 比對。

── 三種音色 ──────────────────────────────────────────────────────────────
  soft        輕點  低通雜訊 + 低頻正弦,慢一點的衰減,鈍
  mechanical  敲擊  極短的暫態 + 高一點的共鳴體,硬
  drop        水滴  向上掃頻的正弦,長一點的尾巴

── 四種角色 ──────────────────────────────────────────────────────────────
一般鍵 / 空白 / 刪除 / 換行。系統音效本來就分這四個角色
(FX_KEYPRESS_STANDARD / SPACEBAR / DELETE / RETURN),自帶音色沒有理由更粗。
角色之間只差音高與長度,不是四種不同的音 —— 它們要聽起來是同一套。

用法:
    python3 scripts/gen_key_sounds.py            # 產生到 res/raw/
    python3 scripts/gen_key_sounds.py --check    # 只比對,不寫檔
    python3 scripts/gen_key_sounds.py --self-test  # 只驗合成本身,不碰 ffmpeg
"""

import argparse
import array
import hashlib
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

SR = 44100
PEAK = 0.72  # 約 -2.9 dBFS。留 headroom,免得 OEM 的音效鏈再加一次增益就削頂。

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_DIR = os.path.join(ROOT, "android", "app", "src", "main", "res", "raw")

# 角色 → (音高倍率, 長度倍率)。順序即 res/raw 的檔名尾巴。
ROLES = {
    "standard": (1.00, 1.00),
    "space": (0.72, 1.15),   # 最大的那顆鍵,聽起來該低一點、鬆一點
    "delete": (0.60, 1.00),  # 鈍,但不要像錯誤音
    "return": (1.35, 1.10),  # 「送出去了」,亮一點
}

TIMBRES = ("soft", "mechanical", "drop")

TAU = 2.0 * math.pi


def _gauss_stream(seed, n):
    """n 個標準常態亂數。

    Box–Muller:兩個均勻亂數 → 兩個獨立的標準常態。只用 random() 這一支,
    因為它是文件裡明文保證「同一個種子永遠給同一串」的那一支。
    u1 取 1.0 - random() 是為了避開 log(0)(random() 回傳 [0, 1))。
    """
    rng = random.Random(seed)
    out = []
    while len(out) < n:
        u1 = 1.0 - rng.random()
        u2 = rng.random()
        r = math.sqrt(-2.0 * math.log(u1))
        theta = TAU * u2
        out.append(r * math.cos(theta))
        out.append(r * math.sin(theta))
    del out[n:]
    return out


def _env(n, attack_ms, decay_ms):
    """指數衰減包絡,前面接一段極短的線性上升(避免爆音)。"""
    tau = decay_ms / 1000.0
    env = [math.exp(-(i / SR) / tau) for i in range(n)]
    a = int(SR * attack_ms / 1000) or 1
    a = min(a, n)
    for i in range(a):
        # linspace(0, 1, a):兩端都取得到,所以分母是 a-1。
        env[i] *= (i / (a - 1)) if a > 1 else 0.0
    # 尾端 4 ms 淡出:指數尾巴不歸零的話,截斷處會有一聲「喀」。
    f = min(int(SR * 0.004), n)
    for k in range(f):
        env[n - f + k] *= 1.0 - (k / (f - 1) if f > 1 else 0.0)
    return env


def _lowpass(x, cutoff):
    """一階 IIR 低通。用最土的作法,免得為了幾條包絡線引進 scipy。"""
    dt = 1.0 / SR
    rc = 1.0 / (TAU * cutoff)
    a = dt / (rc + dt)
    y = [0.0] * len(x)
    acc = 0.0
    for i, v in enumerate(x):
        acc += a * (v - acc)
        y[i] = acc
    return y


def synth(timbre, role, seed):
    pitch, length = ROLES[role]

    if timbre == "soft":
        ms = 46 * length
        n = int(SR * ms / 1000)
        f0 = 520 * pitch
        env = _env(n, 1.2, 11 * length)
        noise = _lowpass(_gauss_stream(seed, n), 1400 * pitch)
        x = [(math.sin(TAU * f0 * (i / SR)) * 0.85 + noise[i] * 3.0) * env[i]
             for i in range(n)]

    elif timbre == "mechanical":
        ms = 40 * length
        n = int(SR * ms / 1000)
        f0 = 900 * pitch
        noise = _gauss_stream(seed, n)
        # 暫態:寬頻,2 ms 就沒了。這一段負責「硬」。
        env_click = _env(n, 0.3, 1.8)
        # 共鳴體:兩個泛音,8 ms。這一段負責「是一顆鍵,不是一聲雜訊」。
        env_body = _env(n, 0.3, 7.5 * length)
        x = []
        for i in range(n):
            t = i / SR
            body = (math.sin(TAU * f0 * t)
                    + 0.45 * math.sin(TAU * f0 * 2.7 * t)) * env_body[i]
            x.append(noise[i] * env_click[i] * 0.55 + body * 0.9)

    elif timbre == "drop":
        ms = 78 * length
        n = int(SR * ms / 1000)
        f0 = 680 * pitch
        t_last = (n - 1) / SR
        env_main = _env(n, 1.0, 20 * length)
        env_tail = _env(n, 0.3, 1.2)
        noise = _lowpass(_gauss_stream(seed, n), 3000)
        # 水滴的特徵是**向上**掃頻:相位要用頻率的積分,直接寫 sin(2πf(t)t)
        # 會得到兩倍的掃頻速度(這是最常見的那個錯)。
        x = []
        acc = 0.0
        for i in range(n):
            t = i / SR
            f = f0 * (1.0 + 1.25 * (t / t_last) ** 1.6)
            acc += f
            phase = TAU * acc / SR
            x.append(math.sin(phase) * env_main[i]
                     + noise[i] * env_tail[i] * 0.35)

    else:
        raise ValueError(timbre)

    peak = max(abs(v) for v in x) if x else 0.0
    if peak > 0:
        scale = PEAK / peak
        x = [v * scale for v in x]
    # int16 的截斷方式與 numpy 的 .astype(np.int16) 一致:朝零取整。
    return array.array("h", (int(v * 32767.0) for v in x))


def pcm_bytes(samples):
    return struct.pack("<%dh" % len(samples), *samples)


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm_bytes(samples))


def decode_ogg(path):
    """把既有的 .ogg 解回 16-bit mono PCM,供 --check 比對。"""
    out = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-f", "s16le", "-ac", "1",
         "-ar", str(SR), "-"],
        stdout=subprocess.PIPE, check=True,
    ).stdout
    a = array.array("h")
    a.frombytes(out[:len(out) - len(out) % 2])
    # ffmpeg 給的是小端序;array 是主機端序。大端序的機器要換過來。
    if sys.byteorder == "big":
        a.byteswap()
    return a


def rms_diff(a, b):
    """兩段 int16 PCM 的均方根差(以滿刻度為 1.0)。"""
    if not a:
        return 0.0
    acc = 0.0
    for i in range(len(a)):
        d = (a[i] - b[i]) / 32768.0
        acc += d * d
    return math.sqrt(acc / len(a))


def rms(samples):
    if not samples:
        return 0.0
    acc = 0.0
    for v in samples:
        d = v / 32768.0
        acc += d * d
    return math.sqrt(acc / len(samples))


def seed_for(ti, ri):
    # 種子固定 = PCM 逐位元可重現。換了公式就換了 PCM,sha256 會告訴你。
    return 0x4C4B + ti * 101 + ri


def self_test():
    """不碰 ffmpeg,只驗合成本身站得住:決定性、長度、峰值、不是靜音。

    這一段存在的理由與整支腳本一樣 —— 守門要能在最貧瘠的機器上跑起來。
    沒有 ffmpeg 的環境(例如只想確認合成沒被改壞)仍然驗得到這幾件事。
    """
    bad = []
    for ti, timbre in enumerate(TIMBRES):
        for ri, role in enumerate(ROLES):
            s1 = synth(timbre, role, seed_for(ti, ri))
            s2 = synth(timbre, role, seed_for(ti, ri))
            name = "%s/%s" % (timbre, role)
            if bytes(pcm_bytes(s1)) != bytes(pcm_bytes(s2)):
                bad.append("%s 同一個種子合出兩份不同的 PCM(決定性壞了)" % name)
            if len(s1) < SR * 0.02:
                bad.append("%s 只有 %d 個取樣,短得不像一顆鍵" % (name, len(s1)))
            peak = max(abs(v) for v in s1)
            want = int(PEAK * 32767)
            if abs(peak - want) > 2:
                bad.append("%s 峰值 %d,期待 %d 附近(正規化壞了)" % (name, peak, want))
            if rms(s1) < 0.01:
                bad.append("%s 幾乎是靜音" % name)
    # 兩個不同的種子不可以合出同一份 PCM(複製貼上會過上面每一條)。
    if bytes(pcm_bytes(synth("soft", "standard", 1))) == \
       bytes(pcm_bytes(synth("soft", "standard", 2))):
        bad.append("換了種子卻是同一份 PCM —— 雜訊根本沒進去")
    for b in bad:
        print("✗ " + b)
    if bad:
        sys.exit(1)
    print("✓ 合成自我測試通過(%d 份,決定性 / 長度 / 峰值 / 非靜音)"
          % (len(TIMBRES) * len(ROLES)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="不寫檔,只確認現有 .ogg 與這支腳本合出來的一致")
    ap.add_argument("--self-test", action="store_true", dest="self_test",
                    help="只驗合成本身(不需要 ffmpeg,也不看 res/raw)")
    args = ap.parse_args()

    if args.self_test:
        self_test()
        return

    if shutil.which("ffmpeg") is None:
        # 離開碼 3 = **這一關跑不起來**,不是「音檔與腳本對不上」。
        # 兩者都必須是紅的,但說法不能混:混了就會有人去改一份其實
        # 沒問題的音檔。呼叫端(verify_key_feedback.py)認這個 3。
        print("找不到 ffmpeg —— --check 需要它把出貨的 .ogg 解回 PCM。\n"
              "這是**環境缺工具**,不是音檔的問題;裝了它再跑一次。\n"
              "  Debian/Ubuntu: sudo apt-get install -y ffmpeg",
              file=sys.stderr)
        sys.exit(3)

    os.makedirs(OUT_DIR, exist_ok=True)
    rows = []
    bad = []
    total = 0

    for ti, timbre in enumerate(TIMBRES):
        for ri, role in enumerate(ROLES):
            samples = synth(timbre, role, seed_for(ti, ri))
            digest = hashlib.sha256(pcm_bytes(samples)).hexdigest()
            name = "key_%s_%s" % (timbre, role)
            ogg = os.path.join(OUT_DIR, name + ".ogg")

            if args.check:
                if not os.path.isfile(ogg):
                    bad.append("%s 不存在" % name)
                    continue
                got = decode_ogg(ogg)
                # 有損編碼:比的是「聽起來是同一個東西」,不是逐位元。
                m = min(len(got), len(samples))
                if m == 0:
                    bad.append("%s 解不出 PCM" % name)
                    continue
                diff = rms_diff(samples[:m], got[:m])
                # libvorbis 會在尾端補一段(實測約 1000 取樣)。那一段必須是
                # 靜音,不然就不只是編碼器的 padding 了。
                tail_rms = rms(got[m:])
                if abs(len(got) - len(samples)) > SR * 0.05:
                    bad.append("%s 長度對不上(差 %d 取樣)"
                               % (name, len(got) - len(samples)))
                elif diff > 0.06 or tail_rms > 0.01:
                    bad.append("%s 波形對不上(rms=%.4f, 尾端 rms=%.4f)"
                               % (name, diff, tail_rms))
                rows.append((name, len(samples), digest, os.path.getsize(ogg)))
                total += os.path.getsize(ogg)
                continue

            with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tf:
                tmp = tf.name
            try:
                write_wav(tmp, samples)
                subprocess.run(
                    ["ffmpeg", "-v", "error", "-y", "-i", tmp,
                     "-c:a", "libvorbis", "-q:a", "1", "-ac", "1", ogg],
                    check=True,
                )
            finally:
                os.unlink(tmp)
            rows.append((name, len(samples), digest, os.path.getsize(ogg)))
            total += os.path.getsize(ogg)

    w = max(len(r[0]) for r in rows)
    print("%-*s  %7s  %9s  %s" % (w, "檔名", "取樣數", "ogg 位元組", "PCM sha256"))
    for name, n, digest, size in rows:
        print("%-*s  %7d  %9d  %s" % (w, name, n, size, digest))
    print("%-*s  %7s  %9d" % (w, "合計", "", total))

    if bad:
        print()
        for b in bad:
            print("✗ " + b)
        sys.exit(1)
    if args.check:
        print("\n✓ %d 份音檔都與腳本合出來的一致" % len(rows))


if __name__ == "__main__":
    main()
