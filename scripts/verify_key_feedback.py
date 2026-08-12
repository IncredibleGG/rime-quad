#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_key_feedback.py —— 手感這一層的**反向**驗證。

`./gradlew test` 會告訴你 FeedbackPlanTest 是綠的。它不會告訴你那些測試
在守什麼 —— 一份「怎麼改都綠」的測試在 CI 上跟真的測試長得一模一樣。

這支腳本做兩件 gradlew 做不到的事:

  1. **音檔對得上腳本。** `res/raw/key_*.ogg` 是出貨的資料,而它的來源是
     `scripts/gen_key_sounds.py`。重新合成一次,與現有檔案解碼後的波形比對 ——
     有人手動塞了一份別的音檔進去,這裡會紅。

  2. **植入六個真缺陷,確認每一個都被抓到。** 其中兩個(M1 / M2)就是
     改動前的實作:三個不相干的觸覺常數、以及一律送 FX_KEYPRESS_STANDARD。
     一個沒有被反向驗證過的測試,一律當作沒有。

用法:  python3 scripts/verify_key_feedback.py
"""
import glob
import io
import os
import subprocess
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ANDROID = os.path.join(ROOT, "android")
PLAN = os.path.join(ANDROID, "app/src/main/java/org/luminakey/ime/core/FeedbackPlan.kt")
LEVELS = os.path.join(ANDROID, "app/src/main/java/org/luminakey/ime/prefs/PrefLevels.kt")

CLASSES = [
    "org.luminakey.ime.core.FeedbackPlanTest",
    "org.luminakey.ime.prefs.FeelPrefsTest",
]

MUTANTS = [
    (
        "M1 震動階梯翻回改動前的三個常數(強比中短)",
        PLAN,
        """    private val DURATION_MS = mapOf(
        HapticStrength.LIGHT to 15,
        HapticStrength.MEDIUM to 20,
        HapticStrength.HEAVY to 25,
    )""",
        """    private val DURATION_MS = mapOf(
        HapticStrength.LIGHT to 101,
        HapticStrength.MEDIUM to 101,
        HapticStrength.HEAVY to 30,
    )""",
    ),
    (
        "M2 系統音效一律送 STANDARD(改動前的實作)",
        PLAN,
        "            SoundTimbre.SYSTEM -> Sound.System(role, v)",
        "            SoundTimbre.SYSTEM -> Sound.System(KeyRole.STANDARD, v)",
    ),
    (
        "M3 自帶素材不分角色",
        PLAN,
        '        "key_" + timbre.name.lowercase() + "_" + role.name.lowercase()',
        '        "key_" + timbre.name.lowercase() + "_standard"',
    ),
    (
        "M4 角色判定永遠回一般鍵",
        PLAN,
        "        val k = keysym?.trim()?.lowercase()",
        '        val k: String? = null\n        @Suppress("UNUSED_EXPRESSION") keysym',
    ),
    (
        "M5 音量 0 仍然出聲",
        PLAN,
        "        if (!enabled || v <= 0f) return Sound.Silent",
        "        if (!enabled) return Sound.Silent",
    ),
    (
        "M6 按鍵音關著時選音色不把音量打開(死控制項)",
        LEVELS,
        "        return if (soundLevel <= 0) withSound(next, 1) else next",
        "        return next",
    ),
]


def gradle():
    args = ["./gradlew", "testDebugUnitTest", "-q"]
    for c in CLASSES:
        args += ["--tests", c]
    p = subprocess.run(args, cwd=ANDROID, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def failed_names():
    out = []
    for f in glob.glob(os.path.join(
            ANDROID, "app/build/test-results/testDebugUnitTest/*.xml")):
        for tc in ET.parse(f).getroot().iter("testcase"):
            if tc.find("failure") is not None or tc.find("error") is not None:
                out.append(tc.get("name"))
    return sorted(out)


def main():
    fails = 0

    print("=== 1. 出貨的音檔與 gen_key_sounds.py 對得上嗎 ===")
    p = subprocess.run([sys.executable, os.path.join(HERE, "gen_key_sounds.py"),
                        "--check"], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    tail = p.stdout.decode("utf-8", "replace").strip().splitlines()
    if p.returncode == 0:
        print("  [PASS] " + tail[-1].strip())
    else:
        fails += 1
        print("  [FAIL] 音檔與腳本對不上")
        for l in tail[-6:]:
            print("         " + l)

    print()
    print("=== 2. 正控:沒動過的時候必須是綠的 ===")
    code, out = gradle()
    if code == 0:
        print("  [PASS] FeedbackPlanTest / FeelPrefsTest 全綠")
    else:
        fails += 1
        print("  [FAIL] 還沒植入任何缺陷就已經是紅的 —— 底下的變異測試不算數")
        print(out[-1500:])
        sys.exit(1)

    print()
    print("=== 3. 植入六個真缺陷,每一個都必須被抓到 ===")
    for name, path, old, new in MUTANTS:
        s = io.open(path, encoding="utf-8").read()
        if s.count(old) != 1:
            fails += 1
            print("  [FAIL] %s —— 找不到要換掉的那一段(實作變了?)" % name)
            continue
        io.open(path, "w", encoding="utf-8").write(s.replace(old, new))
        try:
            code, _ = gradle()
            names = failed_names() if code != 0 else []
        finally:
            io.open(path, "w", encoding="utf-8").write(s)
        if code == 0:
            fails += 1
            print("  [FAIL] %s —— **沒有被抓到**" % name)
        else:
            print("  [PASS] %s(%d 條紅:%s)"
                  % (name, len(names), ", ".join(names[:2])))

    print()
    code, _ = gradle()
    if code != 0:
        fails += 1
        print("  [FAIL] 還原之後還是紅的 —— 這支腳本把原始碼弄壞了")
    else:
        print("  [PASS] 六個變異都還原了,測試回到綠")

    print()
    print("============================================")
    if fails:
        print(" 手感驗證:失敗 %d 項" % fails)
        sys.exit(1)
    print(" 手感驗證:全數通過(1 份音檔比對 + 6 個變異)")


if __name__ == "__main__":
    main()
