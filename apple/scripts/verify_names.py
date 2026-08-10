#!/usr/bin/env python3
"""verify_names.py — 把「必須互相對得上的名字」全部釘在一起。

── 為什麼有這一支 ─────────────────────────────────────────────────────────
改名最典型的失敗是**改了一半**:顯示名改了但識別碼沒改、某個字串是拼接出來
的所以 grep 不到、或某個檔案路徑寫死在腳本裡。而這一類失敗的症狀全都長成
「裝得起來、看得到、但不能用,而且沒有錯誤訊息」:

  · `TISInputSourceID` 沒有以 bundle id 為前綴  → 系統不列出這個輸入模式
  · `.lproj` 的鍵不是輸入模式 id                → 清單顯示的是 bundle id 原文
  · `@objc(...)` 與 plist 的類別名對不上        → 一個字都打不出來
  · `hantSuffix` / `hansSuffix` 對不上          → 選了簡體卻打出繁體字
  · 腳本裡的 `.app` / 執行檔名沒跟著改          → 建得出來但驗不到

`verify_app_bundle.sh` 與 `verify_pkg.sh` 驗的是**建好之後**的產物,而且需要
macOS。這一支驗的是**原始碼裡的名字彼此相符**:不需要編譯、不需要 macOS、
30 毫秒跑完,所以排在 CI 最前面 —— Kit 壞掉時不該讓人等四分鐘的 librime。

⚠ 只用標準函式庫(plistlib / re),**不依賴 PyYAML** —— runner 上有沒有它
  不是我們能保證的事,而「因為缺套件所以跳過」正是這個專案最怕的那種綠。

用法:
    python3 apple/scripts/verify_names.py
    python3 apple/scripts/verify_names.py --self-test   # 反向測試,見下

⚠ **這支腳本自己也要被證明會紅。** 「檢查在該紅的時候安靜地不跑」是這個專案
  最會出事的一類問題(發布關卡的升級測試因步驟順序寫反被判「略過」而報全綠;
  LayoutEscapeTest 的清單寫死四份,12 份裡有 8 份從沒被檢查過)。
  所以 `--self-test` 會**真的把違規植入原始碼**、跑一次自己、斷言紅的是
  對應的那一條、然後還原(`finally` 保證,即使中途爆掉)。
  CI 上先跑 `--self-test` 再跑正向,順序與 verify_single_egress.sh 一致。
"""
import os
import plistlib
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
PKG = os.path.join(ROOT, "apple/LuminaKey")
WORKFLOW = ".github/workflows/macos.yml"

# 規範值。改這裡就要一起改 docs/decisions/product-name.md ——
# 這個表是四端一致的那一份的 macOS 部分。
BUNDLE_ID = "org.luminakey.inputmethod.LuminaKey"
PRODUCT = "LuminaKey"
LEGACY_PRODUCT_RE = r"RimeQuad|rimequad|RIMEQUAD"

fails = []

# ── 反向測試 ──────────────────────────────────────────────────
# (名字, 相對路徑, 原字串, 替換, 期待紅的那一段訊息)
# 一個變異一個地方,所以「哪一條沒在斷言」看得出來是哪一條。
SELF_TEST = [
    ("TISInputSourceID 沒有以 bundle id 為前綴",
     "apple/LuminaKey/Resources/Info.plist",
     "<string>org.luminakey.inputmethod.LuminaKey.Hant</string>\n        <key>TISIntendedLanguage</key>",
     "<string>com.example.NotOurs</string>\n        <key>TISIntendedLanguage</key>",
     "TISInputSourceID == 它自己的鍵"),
    ("可見順序少列了一個輸入模式",
     "apple/LuminaKey/Resources/Info.plist",
     "      <string>org.luminakey.inputmethod.LuminaKey.Hans</string>\n    </array>\n  </dict>",
     "    </array>\n  </dict>",
     "tsVisibleInputModeOrderedArrayKey 與 tsInputModeListKey"),
    ("某個語言少了一個輸入模式的顯示名",
     "apple/LuminaKey/Resources/zh-Hant.lproj/InfoPlist.strings",
     '"org.luminakey.inputmethod.LuminaKey.Hans" = "LuminaKey 簡體";',
     "",
     "zh-Hant.lproj 的 org.luminakey.inputmethod.LuminaKey.Hans 有像樣的顯示名"),
    ("顯示名就是 id 本身(真機回報過的那個缺陷)",
     "apple/LuminaKey/Resources/en.lproj/InfoPlist.strings",
     '"org.luminakey.inputmethod.LuminaKey.Hans" = "LuminaKey (Simplified)";',
     '"org.luminakey.inputmethod.LuminaKey.Hans" = "org.luminakey.inputmethod.LuminaKey.Hans";',
     "en.lproj 的 org.luminakey.inputmethod.LuminaKey.Hans 有像樣的顯示名"),
    ("@objc(...) 與 plist 對不上",
     "apple/LuminaKey/AppSources/LuminaKeyInputController.swift",
     "@objc(LuminaKeyInputController)", "@objc(NopeController)",
     "Swift 端有 @objc(LuminaKeyInputController)"),
    ("mangled 名字裡的長度數字錯了",
     "apple/LuminaKey/Resources/Info.plist",
     "_TtC9LuminaKey24LuminaKeyInputController",
     "_TtC8LuminaKey24LuminaKeyInputController",
     "mangled 名字"),
    ("hansSuffix 與輸入模式 id 對不上",
     "apple/LuminaKey/Sources/LuminaKeyKit/InputModeBinding.swift",
     'hansSuffix = ".Hans"', 'hansSuffix = ".Simplified"', "hansSuffix"),
    ("執行檔改名但驗證腳本沒跟上",
     "apple/scripts/build_app.sh",
     '-o "${APP}/Contents/MacOS/LuminaKey"', '-o "${APP}/Contents/MacOS/LuminaKeyIME"',
     "執行檔叫 LuminaKey"),
    ("R2 主機被當成產品名一起改掉",
     "apple/LuminaKey/SettingsSources/SettingsApp.swift",
     "https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev/rime/schemas/index.json",
     "https://pub-luminakey.r2.dev/rime/schemas/index.json",
     "R2 主機名沒有被當成產品名改掉"),
    ("舊名字漏在一個沒有理由留著它的檔案裡",
     "apple/LuminaKey/Sources/LuminaKeyKit/IPC.swift",
     'public static let requestName = "org.luminakey.ipc.request"',
     'public static let requestName = "org.rimequad.ipc.request"',
     "沒有非預期的舊名字殘留"),
    ("相容舊掛載標記那一段被刪掉",
     "apple/LuminaKey/Sources/LuminaKeyKit/UserPhrases.swift",
     'public static let legacyMarkers = ["# rimequad-managed: custom_phrase v1"]',
     "public static let legacyMarkers: [String] = []",
     "UserPhrases 仍然認得改名前的掛載標記"),
    ("搬遷沒有被叫到(改名 = 使用者詞庫消失)",
     "apple/LuminaKey/AppSources/AppContext.swift",
     "LegacyDataMigration.run(appSupportDir: appSupport)",
     "Optional<Int>.none.map { _ in }",
     "AppContext.swift 會叫搬遷"),
    ("資料目錄又被寫成字面值",
     "apple/LuminaKey/SettingsSources/SettingsApp.swift",
     "appSupportDir.appendingPathComponent(LegacyDataMigration.currentDirectoryName)",
     'appSupportDir.appendingPathComponent("Library/Application Support/LuminaKey")',
     "SettingsApp.swift 不自己拼資料目錄的字面值"),
    ("範本目錄名在 build_app.sh 那一側被改掉(放在 A、去 B 找)",
     "apple/scripts/build_app.sh",
     'mkdir -p "${APP}/Contents/Resources/UserTemplate"',
     'mkdir -p "${APP}/Contents/Resources/UserTemplateX"',
     "build_app.sh 建的是 Resources/UserTemplate"),
    ("範本的複製目的地被改掉(目錄建了,東西沒進去)",
     "apple/scripts/build_app.sh",
     'cp "${ROOT}/core/data/user/${f}" "${APP}/Contents/Resources/UserTemplate/${f}"',
     'cp "${ROOT}/core/data/user/${f}" "${APP}/Contents/Resources/UserTemplateX/${f}"',
     "build_app.sh 複製的目的地也是 Resources/UserTemplate"),
    ("範本又改回「掃一遍那個目錄」(會把 installation.yaml 與測試詞一起打包)",
     "apple/scripts/build_app.sh",
     'TEMPLATE_FILES=( "default.custom.yaml" )',
     'TEMPLATE_FILES=( )',
     "build_app.sh 用白名單挑範本檔"),
    ("補範本那一步被拿掉(全新安裝的方案清單會是錯的)",
     "apple/LuminaKey/AppSources/AppContext.swift",
     "UserDataSeed.run(templateDir: userTemplateDir, userDir: userDataDir)",
     "UserDataSeed.Outcome.noTemplate",
     "AppContext 真的會補範本"),
    ("變異測試的靶被改掉了(靶沒了 = 那一組沒在測)",
     "apple/LuminaKey/Sources/LuminaKeyKit/KeyMapper.swift",
     "public static let unicodeKeysymBase: Int32 = 0x0100_0000",
     "public static let unicodeKeysymBase: Int32 = 0x0100_0001",
     "裡找得到要變異的字串"),
    ("workflow 掉了一關(正向的那一次呼叫)",
     ".github/workflows/macos.yml",
     "run: ./apple/scripts/verify_single_egress.sh\n", "run: true\n",
     "workflow 還有「單一出口(正向)」"),
    ("workflow 不再跑這支檢查",
     ".github/workflows/macos.yml",
     "run: python3 ./apple/scripts/verify_names.py\n", "run: true\n",
     "workflow 還有「本檔自己(正向)」"),
    ("換鍵的舊檔名不再被釘住(升級 = 使用者調過的鍵位消失)",
     "apple/LuminaKey/Tests/LuminaKeyKitTests/KeyRemapTests.swift",
     'XCTAssertEqual(KeyRemapStore.legacyFileName, "rimequad-layouts.json")',
     "XCTAssertTrue(true)",
     "仍然留著舊名字"),
    ("「多帶了東西」那條反向斷言沒有被反向驗",
     ".github/workflows/macos.yml",
     'expect "多打包了 core/layouts/" "✗ 沒有打包 core/layouts"',
     'echo skip',
     "bundle 反向測試至少有 7 個變異"),
]


def self_test():
    me = os.path.abspath(__file__)

    def run():
        r = subprocess.run([sys.executable, me], capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr

    rc, out = run()
    if rc != 0:
        print("!! 還沒植入任何違規,檢查就已經是紅的 —— 先修那個")
        print(out[-3000:])
        return 1

    n_bad = 0
    for i, (label, rel, frm, to, want) in enumerate(SELF_TEST, 1):
        path = os.path.join(ROOT, rel)
        with open(path, encoding="utf-8") as fh:
            orig = fh.read()
        if frm not in orig:
            print("!! [%d/%d] %s —— 在 %s 找不到要變異的字串,這個反向測試本身壞了"
                  % (i, len(SELF_TEST), label, rel))
            n_bad += 1
            continue
        try:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(orig.replace(frm, to, 1))
            rc, out = run()
        finally:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(orig)
        if rc == 0:
            print("!! [%d/%d] %s —— 植入之後檢查竟然還是綠的" % (i, len(SELF_TEST), label))
            n_bad += 1
            continue
        red = [l for l in out.splitlines() if l.startswith("  ✗") and want in l]
        if not red:
            print("!! [%d/%d] %s —— 紅了,但不是因為「%s」:" % (i, len(SELF_TEST), label, want))
            for l in out.splitlines():
                if l.startswith("  ✗"):
                    print("       %s" % l.strip())
            n_bad += 1
            continue
        print("  ✓ [%d/%d] %s → %s" % (i, len(SELF_TEST), label, red[0].strip()))

    rc, _ = run()
    if rc != 0:
        print("!! 還原之後檢查沒有回到綠的 —— 現場沒收拾乾淨")
        n_bad += 1
    print()
    if n_bad:
        print("!! %d 個變異沒有被抓到" % n_bad)
        return 1
    print("%d 個變異全部被抓到,而且紅在對的地方 ✓" % len(SELF_TEST))
    return 0


if "--self-test" in sys.argv[1:]:
    sys.exit(self_test())


def ok(msg):
    print("  ✓ %s" % msg)


def bad(msg):
    print("  ✗ %s" % msg)
    fails.append(msg)


def check(cond, msg):
    ok(msg) if cond else bad(msg)


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as fh:
        return fh.read()


# ── 1. Info.plist ─────────────────────────────────────────────
print("=== 1. Info.plist ===")
with open(os.path.join(PKG, "Resources/Info.plist"), "rb") as fh:
    info = plistlib.load(fh)
with open(os.path.join(PKG, "Resources/Settings-Info.plist"), "rb") as fh:
    sinfo = plistlib.load(fh)

check(info["CFBundleIdentifier"] == BUNDLE_ID,
      "CFBundleIdentifier == %s（實際 %s）" % (BUNDLE_ID, info["CFBundleIdentifier"]))
check(info["CFBundleExecutable"] == PRODUCT,
      "CFBundleExecutable == %s（實際 %s）" % (PRODUCT, info["CFBundleExecutable"]))
# ⚠ `.Settings` 後綴是 main.swift 分岔的條件,不是裝飾。
check(sinfo["CFBundleIdentifier"] == BUNDLE_ID + ".Settings",
      "設定 app 的 bundle id == <bundle id>.Settings（實際 %s）" % sinfo["CFBundleIdentifier"])
check(".Settings" in read("apple/LuminaKey/AppSources/main.swift"),
      "main.swift 用同一個後綴分岔")
check(sinfo["CFBundleExecutable"] == info["CFBundleExecutable"],
      "兩個 bundle 共用同一個執行檔名")
conn = info["InputMethodConnectionName"]
check(conn.startswith(BUNDLE_ID), "InputMethodConnectionName 以 bundle id 起頭:%s" % conn)

# ── 2. 輸入模式 id ────────────────────────────────────────────
print("=== 2. 輸入模式 id ===")
modes = info["ComponentInputModeDict"]["tsInputModeListKey"]
ordered = info["ComponentInputModeDict"]["tsVisibleInputModeOrderedArrayKey"]
mode_ids = sorted(modes.keys())
check(len(mode_ids) >= 1, "至少宣告一個輸入模式")
for mid in mode_ids:
    check(mid.startswith(BUNDLE_ID + "."), "以 bundle id 為前綴:%s" % mid)
    check(modes[mid]["TISInputSourceID"] == mid, "TISInputSourceID == 它自己的鍵:%s" % mid)
check(sorted(ordered) == mode_ids,
      "tsVisibleInputModeOrderedArrayKey 與 tsInputModeListKey 的鍵完全相同")

# ── 3. .lproj 的鍵就是輸入模式 id ─────────────────────────────
print("=== 3. 在地化顯示名 ===")
LANGS = ("en", "zh-Hant", "zh-Hans")
check(sorted(info["CFBundleLocalizations"]) == sorted(LANGS),
      "CFBundleLocalizations == %s" % (LANGS,))
for lang in LANGS:
    text = read("apple/LuminaKey/Resources/%s.lproj/InfoPlist.strings" % lang)
    pairs = dict(re.findall(r'^"([^"]+)"\s*=\s*"([^"]*)"', text, re.M))
    for k in ("CFBundleName", "CFBundleDisplayName"):
        check(bool(pairs.get(k)), "%s.lproj 有 %s" % (lang, k))
    for mid in mode_ids:
        v = pairs.get(mid)
        # 空的或等於 id 本身 = 清單裡顯示一串沒有人敢點的東西。真機回報過。
        check(bool(v) and v != mid,
              "%s.lproj 的 %s 有像樣的顯示名(%r)" % (lang, mid, v))
    check(PRODUCT in (pairs.get("CFBundleName") or ""),
          "%s.lproj 的 CFBundleName 含產品名(%r)" % (lang, pairs.get("CFBundleName")))
    stext = read("apple/LuminaKey/Resources/Settings/%s.lproj/InfoPlist.strings" % lang)
    check("CFBundleName" in stext, "設定 app 的 %s.lproj 有 CFBundleName" % lang)

# ── 4. controller 類別名 ──────────────────────────────────────
print("=== 4. controller 類別名 ===")
ctrl = info["InputMethodServerControllerClass"]
src = read("apple/LuminaKey/AppSources/LuminaKeyInputController.swift")
check("@objc(%s)" % ctrl in src, "Swift 端有 @objc(%s)" % ctrl)
check(re.search(r"final class %s\s*:\s*IMKInputController" % re.escape(ctrl), src) is not None,
      "類別名就是 %s,而且繼承 IMKInputController" % ctrl)
check('"InputMethodServerControllerClass"' in read("apple/LuminaKey/AppSources/SelfCheck.swift"),
      "SelfCheck 仍然照 IMKit 的作法查一次這個類別")

# ⚠ mangled 名字帶著**長度**:_TtC<模組名長度><模組名><類別名長度><類別名>。
#   照字面取代產品名會留下錯的長度,而那個數字只出現在註解裡 ——
#   註解錯了不會壞,但下一個人會照著它去猜,然後猜錯。
build_app = read("apple/scripts/build_app.sh")
module = re.search(r"-module-name\s+(\S+)", build_app).group(1)
want_mangled = "_TtC%d%s%d%s" % (len(module), module, len(ctrl), ctrl)
for rel in ("apple/LuminaKey/AppSources/LuminaKeyInputController.swift",
            "apple/LuminaKey/AppSources/SelfCheck.swift",
            "apple/LuminaKey/Resources/Info.plist",
            "apple/README.md"):
    for found in re.findall(r"_TtC\w+", read(rel)):
        check(found == want_mangled,
              "%s 的 mangled 名字 %s == %s" % (rel, found, want_mangled))

# ── 5. InputModeBinding ───────────────────────────────────────
print("=== 5. InputModeBinding 的後綴 ===")
binding = read("apple/LuminaKey/Sources/LuminaKeyKit/InputModeBinding.swift")
for kind in ("hant", "hans"):
    m = re.search(r'%sSuffix = "([^"]+)"' % kind, binding)
    suf = m.group(1) if m else None
    check(suf is not None and any(mid.endswith(suf) for mid in mode_ids),
          "%sSuffix=%r 命中某一個輸入模式 id" % (kind, suf))

# ── 6. 腳本裡的產物名字 ───────────────────────────────────────
print("=== 6. 產物名字 ===")
vab = read("apple/scripts/verify_app_bundle.sh")
vpkg = read("apple/scripts/verify_pkg.sh")
bpkg = read("apple/scripts/build_pkg.sh")
check('APP="${BUILD}/%s.app"' % PRODUCT in build_app, "build_app.sh 產出 %s.app" % PRODUCT)
check('-o "${APP}/Contents/MacOS/%s"' % PRODUCT in build_app, "執行檔叫 %s" % PRODUCT)
check('BIN="${APP}/Contents/MacOS/%s"' % PRODUCT in vab, "verify_app_bundle.sh 找同一個執行檔")
check('[ "${EXEC}" = "%s" ]' % PRODUCT in vab, "verify_app_bundle.sh 斷言 CFBundleExecutable")
check('PKG_ID="%s.pkg"' % BUNDLE_ID in bpkg, "pkg 識別碼 == <bundle id>.pkg")
check(BUNDLE_ID in vpkg, "verify_pkg.sh 找同一個 bundle id")
settings_app = "%sSettings.app" % PRODUCT
check(settings_app in build_app and settings_app in vpkg
      and settings_app in read("apple/LuminaKey/AppSources/LuminaKeyInputController.swift"),
      "設定 app 的 bundle 檔名 %s 三處一致" % settings_app)
check('hasPrefix("%s.")' % BUNDLE_ID.rsplit(".", 2)[0] in read("apple/scripts/tis_probe.swift"),
      "tis_probe 找 org.luminakey.*")

egress = read("apple/scripts/verify_single_egress.sh")
gate = re.search(r'GATE="([^"]+)"', egress).group(1)
check(os.path.isfile(os.path.join(PKG, gate)), "verify_single_egress.sh 的 GATE 存在:%s" % gate)
esrc = re.search(r'SRC="\$\{ROOT\}/(\S+)"', egress).group(1)
check(os.path.isdir(os.path.join(ROOT, esrc)), "verify_single_egress.sh 的 SRC 存在:%s" % esrc)

# ── 7. 使用者資料目錄與搬遷 ───────────────────────────────────
print("=== 7. 使用者資料與搬遷 ===")
mig = read("apple/LuminaKey/Sources/LuminaKeyKit/LegacyDataMigration.swift")
check('currentDirectoryName = "%s"' % PRODUCT in mig, "新的使用者資料目錄名")
check('legacyDirectoryName = "RimeQuad"' in mig, "認得改名前的目錄名")
check('legacyRegistryFileName = "rimequad-store.json"' in mig, "認得改名前的安裝紀錄檔名")
check('fileName = "luminakey-store.json"'
      in read("apple/LuminaKey/Sources/LuminaKeyKit/InstalledRegistry.swift"),
      "現在的安裝紀錄檔名")
up = read("apple/LuminaKey/Sources/LuminaKeyKit/UserPhrases.swift")
check('"# rimequad-managed: custom_phrase v1"' in up,
      "UserPhrases 仍然認得改名前的掛載標記(否則使用者的詞庫掛載檔會變成「別人的」)")
for rel in ("apple/LuminaKey/AppSources/AppContext.swift",
            "apple/LuminaKey/SettingsSources/SettingsApp.swift"):
    t = read(rel)
    check("LegacyDataMigration.run(" in t, "%s 會叫搬遷" % os.path.basename(rel))
    # 路徑一旦寫成字面值,下次改名就又會漏掉一處。
    check("Library/Application Support/%s" % PRODUCT not in t,
          "%s 不自己拼資料目錄的字面值" % os.path.basename(rel))
# 刻意避開 Squirrel 的目錄 —— 共用使用者詞典會互相踩。
check("~/Library/Rime" in read("apple/LuminaKey/AppSources/AppContext.swift"),
      "「刻意不用 ~/Library/Rime」那段理由還在")

# ── 8. 不該跟著改名的東西 ─────────────────────────────────────
print("=== 8. 不該跟著改名的東西 ===")
# 「Rime」講的是引擎,是對的;R2 的路徑與主機名是發布位置,改了會斷。
R2_HOST = "pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev"
sapp = read("apple/LuminaKey/SettingsSources/SettingsApp.swift")
check("%s/rime/schemas/index.json" % R2_HOST in sapp, "方案索引仍指著真的 R2 位置")
check("pub-%s" % PRODUCT.lower() not in sapp, "R2 主機名沒有被當成產品名改掉")
check("librime" in read("apple/scripts/package_core.sh"), "librime 這個字沒有被取代")
check("rime_console" in read("apple/scripts/package_core.sh"), "rime_console 沒有被取代")
check("rs_init" in read("apple/LuminaKey/AppSources/RimeEngine.swift"), "rs_* 門面沒有被取代")

# ── 9. 舊名字殘留 ─────────────────────────────────────────────
print("=== 9. 舊名字殘留 ===")
# 舊名字只准出現在「它指的就是磁碟上那個既成事實」的地方。
MUST_KEEP = {          # 允許,而且**必須**還在(不見了 = 相容那一段被刪掉了)
    "apple/LuminaKey/Sources/LuminaKeyKit/LegacyDataMigration.swift":
        "改名前的目錄名與檔名",
    "apple/LuminaKey/Tests/LuminaKeyKitTests/LegacyDataMigrationTests.swift":
        "搬遷的測試要造得出舊目錄",
    "apple/LuminaKey/Sources/LuminaKeyKit/UserPhrases.swift":
        "legacyMarkers",
    "apple/LuminaKey/Tests/LuminaKeyKitTests/ConfigAndDictTests.swift":
        "舊標記的測試",
    "apple/LuminaKey/Tests/LuminaKeyKitTests/KeyRemapTests.swift":
        "換鍵的舊檔名(釘住 KeyRemapStore.legacyFileName)",
}
MAY_MENTION = {        # 允許出現在說明文字裡,沒有也可以
    "apple/README.md",
    "apple/LuminaKey/Sources/LuminaKeyKit/KeyRemapStore.swift",
    "apple/LuminaKey/AppSources/AppContext.swift",
    "apple/LuminaKey/SettingsSources/SettingsApp.swift",
    "apple/scripts/verify_names.py",
}
seen, residue = {}, []
targets = [os.path.join(ROOT, "apple"), os.path.join(ROOT, WORKFLOW)]
paths = []
for t in targets:
    if os.path.isfile(t):
        paths.append(t)
        continue
    for base, _dirs, files in os.walk(t):
        if os.sep + "build" in base + os.sep:
            continue
        paths += [os.path.join(base, f) for f in files]
for p in paths:
    rel = os.path.relpath(p, ROOT)
    try:
        with open(p, encoding="utf-8") as fh:
            lines = fh.read().splitlines()
    except (UnicodeDecodeError, IsADirectoryError, FileNotFoundError):
        continue
    for i, line in enumerate(lines, 1):
        if re.search(LEGACY_PRODUCT_RE, line):
            if rel in MUST_KEEP:
                seen[rel] = seen.get(rel, 0) + 1
            elif rel not in MAY_MENTION:
                residue.append("%s:%d: %s" % (rel, i, line.strip()[:100]))
check(not residue, "沒有非預期的舊名字殘留")
for r in residue:
    print("      %s" % r)
for rel, why in sorted(MUST_KEEP.items()):
    check(seen.get(rel, 0) > 0, "%s 仍然留著舊名字(%s)" % (rel, why))

# ── 10. shell 語法 ────────────────────────────────────────────
print("=== 10. shell 語法 ===")
for f in sorted(os.listdir(os.path.join(ROOT, "apple/scripts"))):
    if not f.endswith(".sh"):
        continue
    r = subprocess.run(["bash", "-n", os.path.join(ROOT, "apple/scripts", f)],
                       capture_output=True, text=True)
    check(r.returncode == 0,
          "bash -n apple/scripts/%s%s" % (f, "" if r.returncode == 0 else " — " + r.stderr.strip()))

# ── 11. workflow 沒有掉關卡 ───────────────────────────────────
print("=== 11. workflow ===")
wf = read(WORKFLOW)
for pat, why in [
    (r"apple/build/%s\.app" % PRODUCT, ".app 路徑"),
    (r"apple/build/dist/%s-\*\.pkg" % PRODUCT, ".pkg 路徑"),
    (r"%s-app-\*\.tar\.gz" % PRODUCT, ".app tar 路徑"),
]:
    check(re.search(pat, wf) is not None, "workflow 指到對的 %s" % why)
# ⚠ 只 grep 檔名不夠:它可能只剩在註解裡,或只剩反向測試那一次呼叫,
#   而「正向那一關被拿掉了」看起來跟一切正常一模一樣。連呼叫形態一起比對。
for pat, why in [
    (r"^\s*run: \./apple/scripts/verify_single_egress\.sh --expect-fail\s*$", "單一出口的反向測試"),
    (r"^\s*run: \./apple/scripts/verify_single_egress\.sh\s*$", "單一出口(正向)"),
    (r"^\s*run: \./apple/scripts/run_kit_tests\.sh\s*$", "單元測試 + 變異測試"),
    (r"^\s*run: \./apple/scripts/build_macos\.sh\s*$", "編原生層"),
    (r"^\s*run: \./apple/scripts/verify_data\.sh\s*$", "執行期資料"),
    (r"^\s*run: \./apple/scripts/verify_schema_seed\.sh\s*$", "範本缺席的兩臂對照"),
    (r"\./apple/scripts/verify_console\.sh --expect-fail", "核心層斷言的反向測試"),
    (r"\./apple/scripts/verify_console\.sh nihao", "核心層:拼音"),
    (r"\./apple/scripts/verify_console\.sh su3cl3", "核心層:注音"),
    (r"^\s*run: \./apple/scripts/verify_user_dict\.sh\s*$", "使用者詞庫"),
    (r"^\s*run: \./apple/scripts/build_app\.sh\s*$", "建 .app"),
    (r"^\s*run: \./apple/scripts/verify_app_bundle\.sh\s*$", "bundle 驗證(正向)"),
    (r"\./apple/scripts/verify_app_bundle\.sh --expect-fail", "bundle 驗證的反向測試"),
    (r"^\s*run: \./apple/scripts/build_pkg\.sh\s*$", "做 .pkg"),
    (r"^\s*run: \./apple/scripts/verify_pkg\.sh\s*$", "真的裝一次"),
    (r"^\s*run: \./apple/scripts/package_core\.sh\s*$", "打包核心產物"),
    (r"^\s*run: python3 \./apple/scripts/verify_names\.py --self-test\s*$", "本檔的反向測試"),
    (r"^\s*run: python3 \./apple/scripts/verify_names\.py\s*$", "本檔自己(正向)"),
]:
    check(re.search(pat, wf, re.M) is not None, "workflow 還有「%s」" % why)

# bundle 反向測試的每一個變異都要斷言**不同的**一行 ✗,
# 而且那一行必須真的是 verify_app_bundle.sh 印得出來的。
#
# ⚠ 用**下界**而不是等號:加一個變異不必回來改數字,而刪掉一個照樣會紅。
#   寫等號的話,下一個人加變異時最省事的做法是把數字改大 —— 那時這一條
#   就退化成「數字對不對」而不是「有沒有人偷偷刪掉一關」。
wants = re.findall(r'expect "[^"]*" "(✗ [^"]+)"', wf)
check(len(wants) >= 7, "bundle 反向測試至少有 7 個變異(找到 %d)" % len(wants))
check(len(set(wants)) == len(wants), "每一段期待文字互不相同")
for w in wants:
    check(w[2:] in vab, "verify_app_bundle.sh 真的印得出「%s」" % w[2:])
check(any("core/layouts" in w for w in wants),
      "「多帶了東西」那條反過來的斷言有被反向驗到")

# ── 11b. 使用者初始配置範本的目錄名 ───────────────────────────
# 這個名字同時出現在三個地方,而改一個而不改另外兩個的症狀是
# **靜靜地什麼都不會發生**:.app 裡放在 A,程式去 B 找,守門檢查 C。
# 後果不是「少一個檔案」,是 librime 改照上游 default.yaml 部署 ——
# 使用者看到「設定裡一列方案都沒有勾」。
print("=== 11b. 使用者初始配置範本 ===")
seed = read("apple/LuminaKey/Sources/LuminaKeyKit/UserDataSeed.swift")
m = re.search(r'templateDirectoryName = "([^"]+)"', seed)
check(m is not None, "UserDataSeed 宣告了 templateDirectoryName")
if m:
    tname = m.group(1)
    ba = read("apple/scripts/build_app.sh")
    # ⚠ **不可以只 grep 名字。** build_app.sh 裡這個名字出現兩次(建目錄、複製
    #   目的地),只查「有沒有這個字」的話,把其中一個改掉照樣是綠的 ——
    #   而那正是「.app 裡放在 A、程式去 B 找」的形態。這裡兩個接線點各查一次。
    #   (這一條是被本檔自己的 --self-test 抓出來的:第一版就是只 grep 名字。)
    check('mkdir -p "${APP}/Contents/Resources/%s"' % tname in ba,
          "build_app.sh 建的是 Resources/%s" % tname)
    check('cp "${ROOT}/core/data/user/${f}" "${APP}/Contents/Resources/%s/${f}"' % tname in ba,
          "build_app.sh 複製的目的地也是 Resources/%s" % tname)
    # 白名單,不是掃目錄 —— core/data/user 在 CI 上同時是 librime 的使用者目錄。
    check('TEMPLATE_FILES=( "default.custom.yaml" )' in ba,
          "build_app.sh 用白名單挑範本檔(不掃整個 core/data/user)")
    check("Contents/Resources/%s/default.custom.yaml" % tname in vab,
          "verify_app_bundle.sh 檢查 Resources/%s/default.custom.yaml" % tname)
    ac = read("apple/LuminaKey/AppSources/AppContext.swift")
    check("UserDataSeed.templateDirectoryName" in ac,
          "AppContext 用常數而不是字面值去找範本")
    check("UserDataSeed.run(" in ac,
          "AppContext 真的會補範本(少了它 = 全新安裝的清單是錯的)")
    sa = read("apple/LuminaKey/SettingsSources/SettingsApp.swift")
    check("UserDataSeed.run(" in sa, "設定 app 也會補一次(它可能被直接打開)")

print()

# ── 12. 變異測試的靶 ──────────────────────────────────────────
print("=== 12. 變異測試的靶 ===")
rkt = read("apple/scripts/run_kit_tests.sh")
muts = re.findall(r'^\s*"(Sources/[^|]+)\|([^|]*)\|([^|]*)\|([^"]+)"\s*$', rkt, re.M)
check(len(muts) >= 11, "run_kit_tests.sh 有 %d 個變異" % len(muts))
tests = ""
tdir = os.path.join(PKG, "Tests/LuminaKeyKitTests")
for f in sorted(os.listdir(tdir)):
    if f.endswith(".swift"):
        tests += read(os.path.relpath(os.path.join(tdir, f), ROOT))
for rel, frm, _to, group in muts:
    # bash 雙引號字面值:\" 是引號,\\ 是一個反斜線。
    frm_real = frm.replace('\\"', '"').replace("\\\\", "\\")
    path = os.path.join(PKG, rel)
    check(os.path.isfile(path), "變異目標存在:%s" % rel)
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as fh:
            check(frm_real in fh.read(),
                  "%s 裡找得到要變異的字串(%s)" % (rel, group))
    check("final class %s" % group in tests, "測試裡真的有 %s" % group)

print()
if fails:
    print("!! %d 項失敗" % len(fails))
    sys.exit(1)
print("名字一致性檢查通過 ✓")
