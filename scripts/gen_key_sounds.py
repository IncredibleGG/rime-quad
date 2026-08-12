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

── 決定性 ────────────────────────────────────────────────────────────────
每一個 (音色, 角色) 用一個固定的種子產生雜訊,所以**PCM 是逐位元可重現的**。
腳本會印出每一份 PCM 的 sha256。

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
"""

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

import numpy as np

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


def _env(n, attack_ms, decay_ms):
    """指數衰減包絡,前面接一段極短的線性上升(避免爆音)。"""
    t = np.arange(n) / SR
    a = int(SR * attack_ms / 1000) or 1
    env = np.exp(-t / (decay_ms / 1000.0))
    env[:a] *= np.linspace(0.0, 1.0, a)
    # 尾端 4 ms 淡出:指數尾巴不歸零的話,截斷處會有一聲「喀」。
    f = min(int(SR * 0.004), n)
    env[-f:] *= np.linspace(1.0, 0.0, f)
    return env


def _lowpass(x, cutoff):
    """一階 IIR 低通。用最土的作法,免得為了幾條包絡線引進 scipy。"""
    dt = 1.0 / SR
    rc = 1.0 / (2 * np.pi * cutoff)
    a = dt / (rc + dt)
    y = np.empty_like(x)
    acc = 0.0
    for i, v in enumerate(x):
        acc += a * (v - acc)
        y[i] = acc
    return y


def synth(timbre, role, seed):
    pitch, length = ROLES[role]
    rng = np.random.default_rng(seed)

    if timbre == "soft":
        ms = 46 * length
        n = int(SR * ms / 1000)
        t = np.arange(n) / SR
        f0 = 520 * pitch
        body = np.sin(2 * np.pi * f0 * t) * 0.85
        noise = _lowpass(rng.standard_normal(n), 1400 * pitch) * 3.0
        x = (body + noise) * _env(n, 1.2, 11 * length)

    elif timbre == "mechanical":
        ms = 40 * length
        n = int(SR * ms / 1000)
        t = np.arange(n) / SR
        f0 = 900 * pitch
        # 暫態:寬頻,2 ms 就沒了。這一段負責「硬」。
        click = rng.standard_normal(n) * _env(n, 0.3, 1.8)
        # 共鳴體:兩個泛音,8 ms。這一段負責「是一顆鍵,不是一聲雜訊」。
        body = (
            np.sin(2 * np.pi * f0 * t) + 0.45 * np.sin(2 * np.pi * f0 * 2.7 * t)
        ) * _env(n, 0.3, 7.5 * length)
        x = click * 0.55 + body * 0.9

    elif timbre == "drop":
        ms = 78 * length
        n = int(SR * ms / 1000)
        t = np.arange(n) / SR
        f0 = 680 * pitch
        # 水滴的特徵是**向上**掃頻:相位要用頻率的積分,直接寫 sin(2πf(t)t)
        # 會得到兩倍的掃頻速度(這是最常見的那個錯)。
        f = f0 * (1.0 + 1.25 * (t / t[-1]) ** 1.6)
        phase = 2 * np.pi * np.cumsum(f) / SR
        x = np.sin(phase) * _env(n, 1.0, 20 * length)
        x += _lowpass(rng.standard_normal(n), 3000) * _env(n, 0.3, 1.2) * 0.35

    else:
        raise ValueError(timbre)

    peak = np.max(np.abs(x))
    if peak > 0:
        x = x / peak * PEAK
    return (x * 32767.0).astype(np.int16)


def pcm_bytes(samples):
    return struct.pack("<%dh" % len(samples), *samples.tolist())


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
    return np.frombuffer(out, dtype="<i2")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="不寫檔,只確認現有 .ogg 與這支腳本合出來的一致")
    args = ap.parse_args()

    if shutil.which("ffmpeg") is None:
        sys.exit("找不到 ffmpeg —— 需要它把 WAV 轉成 .ogg(libvorbis)")

    os.makedirs(OUT_DIR, exist_ok=True)
    rows = []
    bad = []
    total = 0

    for ti, timbre in enumerate(TIMBRES):
        for ri, role in enumerate(ROLES):
            # 種子固定 = PCM 逐位元可重現。換了公式就換了 PCM,sha256 會告訴你。
            seed = 0x4C4B + ti * 101 + ri
            samples = synth(timbre, role, seed)
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
                a = samples[:m].astype(np.float64) / 32768.0
                b = got[:m].astype(np.float64) / 32768.0
                rms = float(np.sqrt(np.mean((a - b) ** 2)))
                # libvorbis 會在尾端補一段(實測約 1000 取樣)。那一段必須是
                # 靜音,不然就不只是編碼器的 padding 了。
                tail = got[m:].astype(np.float64) / 32768.0
                tail_rms = float(np.sqrt(np.mean(tail ** 2))) if len(tail) else 0.0
                if abs(len(got) - len(samples)) > SR * 0.05:
                    bad.append("%s 長度對不上(差 %d 取樣)"
                               % (name, len(got) - len(samples)))
                elif rms > 0.06 or tail_rms > 0.01:
                    bad.append("%s 波形對不上(rms=%.4f, 尾端 rms=%.4f)"
                               % (name, rms, tail_rms))
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
