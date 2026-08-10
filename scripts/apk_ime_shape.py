#!/usr/bin/env python3
"""apk_ime_shape.py —— 發出去的那一份 APK,系統還認不認得它是一個輸入法?

── 為什麼要有這一支 ───────────────────────────────────────────────────────
2026-08-10 使用者回報:「在模擬器下沒有這個輸入法,已經安裝了」,而**同一版
在他的真手機上是好的**。查證的結論是那一份 APK 本身沒有問題(兩個 ABI 都在、
IME service 也在,見 commit 訊息裡的實測紀錄)——
但查證的過程把 release_check.sh 第 4 關的兩個洞挖了出來,兩個都是這個專案
反覆吃虧的同一種毛病:**拿「這個字串有沒有出現在檔案裡」當成「這件事成立」**。

洞 1 —— ABI 只問「lib/<abi>/ 底下有沒有東西」
    舊寫法:`unzip -l "$APK" | grep -oE 'lib/[a-z0-9_-]+/'`。
    但 APK 的 lib/x86_64/ 底下本來就躺著相依函式庫的 .so
    (androidx.graphics.path、datastore_shared_counter)。也就是說
    **我們自己的 JNI .so 從某個 ABI 整個消失,這一關照樣綠**,
    而那正好就是「x86_64 裝置裝得起來、一個字都打不出來」的形狀。

洞 2 —— IME 宣告只問三個字串在不在整份 manifest 裡
    舊寫法把 `aapt2 dump xmltree` 的**整份輸出**當一條字串,比對
    BIND_INPUT_METHOD / android.view.InputMethod / android.view.im。
    這三個字串各自掛在哪個元素上、是不是同一個 <service>、那個 service
    是不是我們的、exported 是不是 true、enabled 有沒有被關掉 —— 一律沒問。
    把真的 <service> 整個拿掉、另外擺一個帶著同樣三個字串的 <receiver>,
    系統眼裡這個 app 就不再是輸入法了,而舊的那一關**三項全綠**。
    這不是假想:`--self-test` 的 legacy_substring_verdict() 就是把舊寫法
    原樣搬進來,並斷言它在那個變異上真的說「綠」。

── 這一支的做法 ─────────────────────────────────────────────────────────
判斷全部抽成**純函式**:吃文字(aapt2 的 xmltree / badging)與一份
(檔名, 位元組數) 清單,吐「哪幾條不成立」。純函式才驗得動 —— shell 的
`case "$X" in *字串*)` 沒辦法寫反向測試,而那正是上面兩個洞活這麼久的原因。

每一項都有**兩個獨立的判讀來源**(這個專案第 3c 關已經用過同一招):
  · ABI:zip 目錄裡真的有那個檔 **且** badging 的 native-code 那一行也承認
  · IME:manifest 樹上結構成立 **且** badging 印得出 provides-component:'ime'
偵測方法自己壞掉的那天(aapt2 換一版少印一行),兩邊會分岔而不是一起變綠。

── 用法 ─────────────────────────────────────────────────────────────────
    apk_ime_shape.py --apk <path> [--aapt2 <path>]   # 驗一份 APK
    apk_ime_shape.py --self-test                     # 反向測試(CI 每次跑)

輸出一行一項,`PASS: ` / `FAIL: `;有任何一項 FAIL 就以 1 結束。
"""

import argparse
import os
import subprocess
import sys
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "lib"))
import product  # noqa: E402  產品識別碼只有一份,見 scripts/lib/product.env

ANDROID_NS = "http://schemas.android.com/apk/res/android"

#: 系統把一個 service 認成輸入法的三個必要條件(AOSP InputMethodInfo 的建構子
#: 與 InputMethodManagerService.queryInputMethodServicesLocked 各自要求一項)。
BIND_PERMISSION = "android.permission.BIND_INPUT_METHOD"
IME_ACTION = "android.view.InputMethod"
IME_META_NAME = "android.view.im"

#: 模擬器(x86_64)與真機(arm64-v8a)各一。少了任何一個,那一類裝置就是
#: 「裝得起來、載不到 .so」。x86_64 不只是模擬器 —— ChromeOS 與部分平板也是。
REQUIRED_ABIS = ("arm64-v8a", "x86_64")

#: JNI 函式庫的檔名(CMakeLists 的 add_library(rime_jni SHARED …))。
JNI_SONAME = "librime_jni.so"

#: JNI .so 的位元組數下界。
#:
#: 這一條擋的不是「檔案在不在」而是「連進去的是真的 librime 還是 stub」。
#: cpp/CMakeLists.txt 在 third_party/prebuilt/<abi>/lib/librime.a 不存在時會
#: **改編 rime_shell_stub.cc**,只印一行 CMake WARNING,建置照樣成功 ——
#: 產出的是一個「鍵盤畫得出來、候選字是假的」的輸入法,而目前沒有任何一關
#: 在看這件事。
#:
#: 量出來的值(2026-08-10,commit 605936d):
#:   release  arm64-v8a 5,934,984   x86_64 5,951,944
#:   debug    arm64-v8a 6,085,424   x86_64 6,098,960
#: stub 版連 librime 都沒連,是幾十 KB 的量級。1 MiB 的下界離兩邊都夠遠。
MIN_JNI_BYTES = 1 << 20


# ─────────────────────────── xmltree 解析(純函式)───────────────────────────


class Node:
    """`aapt2 dump xmltree` 裡的一個元素。"""

    __slots__ = ("name", "attrs", "children")

    def __init__(self, name):
        self.name = name
        self.attrs = {}
        self.children = []

    def find(self, name):
        return [c for c in self.children if c.name == name]

    def attr(self, name, ns=ANDROID_NS):
        return self.attrs.get((ns, name))

    def __repr__(self):  # pragma: no cover - 只給除錯用
        return f"<{self.name} {self.attrs!r} {len(self.children)} children>"


def _split_attr(line):
    """`A: <ns>:<name>(0x…)=<value>` → ((ns, name), value)。

    值可能是三種形狀,三種都要吃得下:
        ="org.example.Foo" (Raw: "org.example.Foo")   字串(帶原始值)
        =true                                          布林/整數
        =@0x7f0e0000                                   資源參考
    """
    body = line[len("A: "):]
    eq = body.find("=")
    if eq < 0:
        return None
    lhs, value = body[:eq], body[eq + 1:]

    # `(0x01010003)` 這種 attribute id 尾巴去掉。
    paren = lhs.rfind("(0x")
    if paren >= 0 and lhs.endswith(")"):
        lhs = lhs[:paren]

    if ":" in lhs:
        ns, name = lhs.rsplit(":", 1)
    else:
        ns, name = None, lhs

    # ` (Raw: …)` 是同一個值的原始寫法,不是值的一部分。
    raw = value.find(' (Raw: ')
    if raw >= 0:
        value = value[:raw]
    value = value.strip()
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        value = value[1:-1]
    return (ns, name), value


def parse_xmltree(text):
    """把 `aapt2 dump xmltree` 的輸出解析成一棵樹,回傳根元素(manifest)。

    格式的規律(實測 aapt2 35.0.0):元素縮排 i 的話,它的屬性在 i+2、
    子元素在 i+4。所以只要靠縮排維護一個堆疊就夠,不必真的懂 XML。
    找不到根元素回 None —— **不是**回一棵空樹。空樹會讓下游每一條檢查都
    「找不到問題」,那是最糟的失敗方式。
    """
    root = None
    stack = []  # [(indent, Node)]
    for raw in text.splitlines():
        stripped = raw.lstrip(" ")
        indent = len(raw) - len(stripped)
        if stripped.startswith("E: "):
            name = stripped[len("E: "):].split(" ", 1)[0]
            node = Node(name)
            while stack and stack[-1][0] >= indent:
                stack.pop()
            if stack:
                stack[-1][1].children.append(node)
            elif root is None:
                root = node
            else:
                # 第二個根元素:格式和我們以為的不一樣,寧可讓它壞得明顯。
                raise ValueError("xmltree 出現第二個根元素,解析器的假設不成立")
            stack.append((indent, node))
        elif stripped.startswith("A: ") and stack:
            got = _split_attr(stripped)
            if got:
                key, value = got
                stack[-1][1].attrs[key] = value
    return root


# ─────────────────────────── 檢查(純函式)───────────────────────────


def absolute_class(name, app_id):
    """manifest 裡的 `android:name` → 完整類別名。`.Foo` 是相對於套件名的。"""
    if not name:
        return None
    if name.startswith("."):
        return app_id + name
    if "." not in name:
        return f"{app_id}.{name}"
    return name


def check_ime_service(root, app_id, ime_service):
    """manifest 樹 → 「輸入法宣告不成立」的原因清單(空清單 = 成立)。

    每一條檢查對應 AOSP 裡一個會讓輸入法**從系統清單上消失**的判斷:
      · 沒有 service           → queryIntentServices 查不到
      · permission 不對        → IMMS 明文 skip(「it does not require…」)
      · exported 不是 true     → 系統綁不上來
      · enabled=false          → 元件被停用
      · 沒有 intent-filter     → 同第一條
      · meta-data 不是資源參考 → InputMethodInfo 建構子丟例外,IMMS 記一行
                                 「Unable to load input method」然後跳過
    症狀全部一樣:app 裝得起來、圖示也在,但「螢幕鍵盤」清單裡沒有它。
    """
    problems = []
    if root is None:
        return ["manifest 解析不出根元素 —— 這一關本身壞了,不是通過"]
    if root.name != "manifest":
        return [f"manifest 的根元素是 {root.name},不是 manifest"]

    apps = root.find("application")
    if len(apps) != 1:
        return [f"<application> 有 {len(apps)} 個,預期 1 個"]
    app = apps[0]
    if app.attr("enabled") == "false":
        problems.append("<application android:enabled=\"false\"> —— 整個 app 的元件都被停用")

    want = absolute_class(ime_service, app_id)
    services = app.find("service")
    mine = [
        s for s in services
        if absolute_class(s.attr("name"), app_id) == want
    ]
    if not mine:
        seen = [absolute_class(s.attr("name"), app_id) or "(無名)" for s in services]
        problems.append(
            f"<application> 底下找不到 <service android:name=\"{want}\">"
            f"(現有的 service:{seen or '一個都沒有'})"
        )
        return problems
    if len(mine) > 1:
        problems.append(f"有 {len(mine)} 個 <service> 都叫 {want}")
    svc = mine[0]

    perm = svc.attr("permission")
    if perm != BIND_PERMISSION:
        problems.append(
            f"IME service 的 android:permission 是 {perm!r},必須是 {BIND_PERMISSION!r}"
            " —— 不符時 IMMS 會直接跳過它"
        )
    if svc.attr("exported") != "true":
        problems.append(
            f"IME service 的 android:exported 是 {svc.attr('exported')!r},必須是 \"true\""
        )
    if svc.attr("enabled") == "false":
        problems.append("IME service 的 android:enabled 是 \"false\"")

    actions = [
        a.attr("name")
        for f in svc.find("intent-filter")
        for a in f.find("action")
    ]
    if IME_ACTION not in actions:
        problems.append(
            f"IME service 的 <intent-filter> 裡沒有 action {IME_ACTION}(現有:{actions})"
        )

    metas = [m for m in svc.find("meta-data") if m.attr("name") == IME_META_NAME]
    if not metas:
        problems.append(f"IME service 底下沒有 <meta-data android:name=\"{IME_META_NAME}\">")
    else:
        res = metas[0].attr("resource")
        if not res or not res.startswith("@"):
            problems.append(
                f"{IME_META_NAME} 的 android:resource 是 {res!r} —— 必須是資源參考(@…),"
                "指向 res/xml/method.xml"
            )
    return problems


def check_native_libs(entries, required_abis=REQUIRED_ABIS, soname=JNI_SONAME,
                      min_bytes=MIN_JNI_BYTES):
    """(檔名 → 位元組數) → 「原生程式庫不成立」的原因清單。

    ⚠ 問的是 **`lib/<abi>/<我們自己的 .so>`**,不是「lib/<abi>/ 底下有沒有東西」。
    相依函式庫會在每個 ABI 底下各放一個 .so,所以後者永遠成立。
    """
    problems = []
    if not entries:
        return ["APK 裡一個檔案都讀不到 —— 這一關本身壞了,不是通過"]
    for abi in required_abis:
        path = f"lib/{abi}/{soname}"
        if path not in entries:
            others = sorted(n for n in entries if n.startswith(f"lib/{abi}/"))
            problems.append(
                f"缺 {path}(lib/{abi}/ 底下有的是:{others or '什麼都沒有'})"
                f" —— {abi} 的裝置裝得起來,但載不到引擎"
            )
            continue
        size = entries[path]
        if size < min_bytes:
            problems.append(
                f"{path} 只有 {size} 位元組(下界 {min_bytes})"
                " —— 這個大小是 stub 建置的量級,表示 librime 沒有連進去"
            )
    return problems


def parse_badging_native_code(badging):
    """badging 的 `native-code: 'a' 'b'` → ['a', 'b']。沒有那一行回 []。"""
    for line in badging.splitlines():
        if line.startswith("native-code:"):
            return [p.strip("'") for p in line[len("native-code:"):].split() if p.strip("'")]
    return []


def check_badging_ime(badging):
    """第二個判讀來源:aapt2 自己認不認為這是輸入法。

    `provides-component:'ime'` 是 aapt2 走過 manifest 之後下的結論,與上面
    那棵樹是兩條獨立的路徑。兩邊都說有,才算數 —— 只問一邊的話,判讀方法
    自己壞掉的那天(aapt2 換一版改了輸出)這一關會安靜地永遠說「好」。
    """
    if not badging.strip():
        return ["aapt2 dump badging 什麼都沒印 —— 這一關本身壞了,不是通過"]
    if "provides-component:'ime'" not in badging:
        return ["badging 沒有 provides-component:'ime' —— aapt2 不認為這份 APK 提供輸入法"]
    return []


def check_badging_abis(badging, required_abis=REQUIRED_ABIS):
    """第二個判讀來源:aapt2 眼裡這份 APK 支援哪些 ABI。"""
    if not badging.strip():
        return ["aapt2 dump badging 什麼都沒印 —— 這一關本身壞了,不是通過"]
    listed = parse_badging_native_code(badging)
    missing = [a for a in required_abis if a not in listed]
    if missing:
        return [f"badging 的 native-code 少了 {missing}(它列的是 {listed})"]
    return []


def check_badging(badging, required_abis=REQUIRED_ABIS):
    """上面兩條的聯集(反向測試用)。"""
    return check_badging_ime(badging) + check_badging_abis(badging, required_abis)


# ─────────────────────────── 舊寫法(只給反向測試當對照組)───────────────────


def legacy_substring_verdict(xmltree_text, zip_names):
    """2026-08-10 之前 release_check.sh 第 4 關的判斷,原樣搬過來。

    留著它**不是**為了還在用,是為了讓「舊的那一關在這個變異上是綠的」變成
    一句被斷言的事實而不是一句宣稱。少了這個對照組,新檢查到底比舊的多擋了
    什麼,只有寫的人自己知道。

    回 True = 舊寫法認為這份 APK 沒問題。
    """
    abis = {n.split("/")[1] for n in zip_names if n.startswith("lib/") and n.count("/") >= 2}
    if "arm64-v8a" not in abis or "x86_64" not in abis:
        return False
    return all(s in xmltree_text for s in (BIND_PERMISSION, IME_ACTION, IME_META_NAME))


# ─────────────────────────── CLI ───────────────────────────


def read_apk(path, aapt2):
    with zipfile.ZipFile(path) as zf:
        entries = {i.filename: i.file_size for i in zf.infolist()}
    xmltree = subprocess.run(
        [aapt2, "dump", "xmltree", "--file", "AndroidManifest.xml", path],
        capture_output=True, text=True, check=False,
    ).stdout
    badging = subprocess.run(
        [aapt2, "dump", "badging", path],
        capture_output=True, text=True, check=False,
    ).stdout
    return entries, xmltree, badging


def run_checks(entries, xmltree, badging, app_id, ime_service):
    """→ [(是否通過, 說明), …]。**一項就是 release_check.sh 的一關**,
    所以刻意逐個 ABI 分開列 —— 合成一項的話,一個 ABI 沒了只會讓某一關的
    文字變長,不會讓關卡數變少,而關卡數正是那支腳本用來發現「整段沒跑到」
    的最後一道防線。"""
    results = []

    for abi in REQUIRED_ABIS:
        problems = check_native_libs(entries, required_abis=(abi,))
        results.append((
            not problems,
            f"lib/{abi}/{JNI_SONAME} 在,而且不是 stub 大小"
            if not problems else "; ".join(problems),
        ))

    ime_problems = check_ime_service(parse_xmltree(xmltree), app_id, ime_service)
    results.append((
        not ime_problems,
        "IME 的四個必要條件(permission／exported／intent-filter／meta-data)"
        "都掛在同一個 <service> 上",
    ) if not ime_problems else (False, "; ".join(ime_problems)))

    ime_badging = check_badging_ime(badging)
    results.append((
        not ime_badging,
        "第二個判讀來源:aapt2 badging 也印得出 provides-component:'ime'"
        if not ime_badging else "; ".join(ime_badging),
    ))

    abi_badging = check_badging_abis(badging)
    results.append((
        not abi_badging,
        f"第二個判讀來源:badging 的 native-code 列出 {'、'.join(REQUIRED_ABIS)}"
        if not abi_badging else "; ".join(abi_badging),
    ))
    return results


# ─────────────────────────── 反向測試 ───────────────────────────

APP_ID = product.ANDROID_APP_ID
IME_SERVICE = product.ANDROID_IME_SERVICE
SERVICE_CLASS = absolute_class(IME_SERVICE, APP_ID)


def _fixture_xmltree(service_block=None):
    """一份形狀與真的 aapt2 輸出一致的 manifest 樹(縮排、attribute id 都照抄)。

    ⚠ 產品識別碼一律從 product.py 帶進來,不寫死 —— 寫死的話
    scripts/verify_product_ids.sh 第 3 關會紅,而它紅得有道理:
    測試資料裡的舊名字和別處的舊名字一樣會腐爛。
    """
    if service_block is None:
        service_block = f"""\
          E: service (line=133)
            A: {ANDROID_NS}:label(0x01010001)=@0x7f0b00ff
            A: {ANDROID_NS}:name(0x01010003)="{IME_SERVICE}" (Raw: "{IME_SERVICE}")
            A: {ANDROID_NS}:permission(0x01010006)="{BIND_PERMISSION}" (Raw: "{BIND_PERMISSION}")
            A: {ANDROID_NS}:exported(0x01010010)=true
              E: intent-filter (line=138)
                  E: action (line=139)
                    A: {ANDROID_NS}:name(0x01010003)="{IME_ACTION}" (Raw: "{IME_ACTION}")
              E: meta-data (line=142)
                A: {ANDROID_NS}:name(0x01010003)="{IME_META_NAME}" (Raw: "{IME_META_NAME}")
                A: {ANDROID_NS}:resource(0x01010025)=@0x7f0e0000
"""
    return f"""\
N: android={ANDROID_NS} (line=2)
  E: manifest (line=2)
    A: {ANDROID_NS}:versionCode(0x0101021b)=26080921
    A: package="{APP_ID}" (Raw: "{APP_ID}")
      E: uses-sdk (line=7)
        A: {ANDROID_NS}:minSdkVersion(0x0101020c)=26
      E: application (line=87)
        A: {ANDROID_NS}:name(0x01010003)="{APP_ID}.RimeApp" (Raw: "{APP_ID}.RimeApp")
          E: activity (line=99)
            A: {ANDROID_NS}:name(0x01010003)="{APP_ID}.MainActivity" (Raw: "{APP_ID}.MainActivity")
            A: {ANDROID_NS}:exported(0x01010010)=true
              E: intent-filter (line=103)
                  E: action (line=104)
                    A: {ANDROID_NS}:name(0x01010003)="android.intent.action.MAIN" (Raw: "android.intent.action.MAIN")
{service_block}\
          E: provider (line=147)
            A: {ANDROID_NS}:name(0x01010003)="androidx.startup.InitializationProvider" (Raw: "androidx.startup.InitializationProvider")
            A: {ANDROID_NS}:exported(0x01010010)=false
"""


def _fixture_entries():
    return {
        "AndroidManifest.xml": 8000,
        "classes.dex": 9_000_000,
        "assets/rime/shared/default.yaml": 4000,
        "lib/arm64-v8a/libandroidx.graphics.path.so": 10096,
        "lib/arm64-v8a/libdatastore_shared_counter.so": 7112,
        "lib/arm64-v8a/librime_jni.so": 5_934_984,
        "lib/x86_64/libandroidx.graphics.path.so": 10760,
        "lib/x86_64/libdatastore_shared_counter.so": 6224,
        "lib/x86_64/librime_jni.so": 5_951_944,
    }


_FIXTURE_BADGING = """\
package: name='{app}' versionCode='26080921' versionName='x'
minSdkVersion:'26'
targetSdkVersion:'35'
application-label:'x'
launchable-activity: name='{app}.MainActivity'  label='x' icon=''
provides-component:'ime'
main
other-activities
supports-screens: 'small' 'normal' 'large' 'xlarge'
native-code: 'arm64-v8a' 'x86_64'
""".format(app=APP_ID)


def _problems(entries=None, xmltree=None, badging=None):
    entries = _fixture_entries() if entries is None else entries
    xmltree = _fixture_xmltree() if xmltree is None else xmltree
    badging = _FIXTURE_BADGING if badging is None else badging
    out = []
    out += check_native_libs(entries)
    out += check_ime_service(parse_xmltree(xmltree), APP_ID, IME_SERVICE)
    out += check_badging(badging)
    return out


def self_test():
    """每一條變異都必須紅,而且**要紅在它自己那一條上**;不變異必須全綠。

    「全部都紅」和「全部都綠」一樣沒有價值 —— 一個永遠回傳問題的檢查會通過
    每一條變異測試。所以正控(第 0 條)與「紅的理由對不對」兩件事都要斷言。
    """
    cases = []   # (名稱, 產生 problems 的 lambda, 期望訊息裡出現的字串)

    # ── 0. 正控:沒有動任何東西,一條問題都不該有 ──────────────────────
    base = _problems()
    ok0 = not base
    print(("PASS" if ok0 else "FAIL")
          + ": [正控] 未變異的 fixture 沒有任何問題"
          + ("" if ok0 else f" —— 卻報了 {base}"))

    # ── 1. 我們自己的 .so 從 x86_64 消失(相依函式庫的 .so 還在)──────────
    def m_missing_so():
        e = _fixture_entries()
        del e["lib/x86_64/librime_jni.so"]
        return e
    cases.append((
        f"x86_64 少了 {JNI_SONAME},但 lib/x86_64/ 底下還有別人的 .so",
        lambda: _problems(entries=m_missing_so()),
        f"缺 lib/x86_64/{JNI_SONAME}",
    ))

    # ── 2. stub 建置(檔案在,但沒連 librime)───────────────────────────
    def m_stub_so():
        e = _fixture_entries()
        e["lib/x86_64/librime_jni.so"] = 42_000
        return e
    cases.append((
        "x86_64 的 .so 是 stub 大小",
        lambda: _problems(entries=m_stub_so()),
        "stub 建置的量級",
    ))

    # ── 3. **這一條是重點**:service 整個消失,三個關鍵字被搬到別的元件上 ──
    decoy = f"""\
          E: receiver (line=133)
            A: {ANDROID_NS}:name(0x01010003)="{APP_ID}.Decoy" (Raw: "{APP_ID}.Decoy")
            A: {ANDROID_NS}:permission(0x01010006)="{BIND_PERMISSION}" (Raw: "{BIND_PERMISSION}")
            A: {ANDROID_NS}:exported(0x01010010)=true
              E: intent-filter (line=138)
                  E: action (line=139)
                    A: {ANDROID_NS}:name(0x01010003)="{IME_ACTION}" (Raw: "{IME_ACTION}")
              E: meta-data (line=142)
                A: {ANDROID_NS}:name(0x01010003)="{IME_META_NAME}" (Raw: "{IME_META_NAME}")
                A: {ANDROID_NS}:resource(0x01010025)=@0x7f0e0000
"""
    cases.append((
        "IME service 被刪掉,三個關鍵字改掛在一個 receiver 上",
        lambda: _problems(xmltree=_fixture_xmltree(decoy),
                          badging=_FIXTURE_BADGING.replace("provides-component:'ime'\n", "")),
        "找不到 <service",
    ))

    # ── 4–8. service 還在,但某一個必要條件被拿掉 ────────────────────────
    def svc_without(attr_line):
        full = _fixture_xmltree()
        return full.replace(attr_line, "")

    cases.append((
        "service 少了 android:permission",
        lambda: _problems(xmltree=svc_without(
            f'            A: {ANDROID_NS}:permission(0x01010006)="{BIND_PERMISSION}" (Raw: "{BIND_PERMISSION}")\n')),
        "android:permission",
    ))
    cases.append((
        "service 的 exported 是 false",
        lambda: _problems(xmltree=_fixture_xmltree().replace(
            f'            A: {ANDROID_NS}:exported(0x01010010)=true\n'
            f'              E: intent-filter (line=138)',
            f'            A: {ANDROID_NS}:exported(0x01010010)=false\n'
            f'              E: intent-filter (line=138)')),
        "android:exported",
    ))
    cases.append((
        "service 被 android:enabled=false 停用",
        lambda: _problems(xmltree=_fixture_xmltree().replace(
            f'            A: {ANDROID_NS}:exported(0x01010010)=true\n'
            f'              E: intent-filter (line=138)',
            f'            A: {ANDROID_NS}:exported(0x01010010)=true\n'
            f'            A: {ANDROID_NS}:enabled(0x0101000e)=false\n'
            f'              E: intent-filter (line=138)')),
        "android:enabled",
    ))
    cases.append((
        "intent-filter 的 action 換成別的",
        lambda: _problems(xmltree=_fixture_xmltree().replace(IME_ACTION, "android.view.NotAnIme")),
        "沒有 action",
    ))
    cases.append((
        "android.view.im 的 meta-data 不是資源參考",
        lambda: _problems(xmltree=_fixture_xmltree().replace(
            f'                A: {ANDROID_NS}:resource(0x01010025)=@0x7f0e0000',
            f'                A: {ANDROID_NS}:value(0x01010024)="method" (Raw: "method")')),
        "android:resource",
    ))

    # ── 9–10. 第二個判讀來源自己出事 ─────────────────────────────────────
    cases.append((
        "badging 不再印 provides-component:'ime'",
        lambda: _problems(badging=_FIXTURE_BADGING.replace("provides-component:'ime'\n", "")),
        "provides-component",
    ))
    cases.append((
        "badging 的 native-code 少了 x86_64",
        lambda: _problems(badging=_FIXTURE_BADGING.replace(
            "native-code: 'arm64-v8a' 'x86_64'", "native-code: 'arm64-v8a'")),
        "native-code",
    ))

    # ── 11. 解析器本身失效時要紅,不可以「找不到問題」──────────────────
    cases.append((
        "xmltree 是空的(aapt2 失敗)",
        lambda: _problems(xmltree=""),
        "這一關本身壞了",
    ))
    cases.append((
        "APK 一個檔案都讀不到",
        lambda: _problems(entries={}),
        "這一關本身壞了",
    ))

    bad = 0 if ok0 else 1
    for name, run, want in cases:
        got = run()
        hit = any(want in p for p in got)
        good = bool(got) and hit
        if not good:
            bad += 1
        print(("PASS" if good else "FAIL") + f": [變異] {name}"
              + ("" if good else f" —— 期望訊息含 {want!r},實得 {got}"))

    # ── 12. 舊寫法在第 3 條變異上是綠的(證明新檢查真的多擋了東西)────────
    legacy_green = legacy_substring_verdict(_fixture_xmltree(decoy),
                                            set(_fixture_entries()))
    legacy_green_on_missing_so = legacy_substring_verdict(
        _fixture_xmltree(), set(m_missing_so()))
    for label, green in (
        ("service 被換成 receiver", legacy_green),
        (f"x86_64 少了 {JNI_SONAME}", legacy_green_on_missing_so),
    ):
        if green:
            print(f"PASS: [對照組] 舊的子字串寫法在「{label}」上仍然說綠 —— 這正是它擋不住的")
        else:
            bad += 1
            print(f"FAIL: [對照組] 舊寫法在「{label}」上已經會紅了 ——"
                  " 那這一條變異證明不了新檢查更嚴,對照組要重挑")

    # ── 13. run_checks 吐幾項 = release_check.sh 第 4 關數幾關 ─────────────
    # 把五項合併成一項不會讓任何一條變紅,只會讓 release_check.sh 的關卡總數
    # 悄悄變小 —— 而那支腳本的下界正是拿來抓「整段沒跑到」的。釘住。
    n_items = len(run_checks(_fixture_entries(), _fixture_xmltree(),
                             _FIXTURE_BADGING, APP_ID, IME_SERVICE))
    if n_items == 5:
        print("PASS: [結構] run_checks 吐 5 項(= release_check.sh 第 4 關的 5 關)")
    else:
        bad += 1
        print(f"FAIL: [結構] run_checks 吐了 {n_items} 項,預期 5 項 ——"
              " release_check.sh 的關卡數下界會跟著鬆掉")

    # 條數的下界。整段被刪掉不會讓任何一項變紅,只會讓總數變小 ——
    # 這個專案在 release_check.sh 上踩過同一個坑。
    total = 1 + len(cases) + 2 + 1
    expect_min = 16   # 量出來的:1 正控 + 12 變異 + 2 對照組 + 1 結構
    if total < expect_min:
        bad += 1
        print(f"FAIL: 這一輪只跑了 {total} 條,少於下界 {expect_min} 條 —— 有東西被刪掉了")

    print(f"\n{total} 條,失敗 {bad} 條")
    return 1 if bad else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--apk")
    ap.add_argument("--aapt2", default="aapt2")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.apk:
        ap.error("要嘛 --apk <path>,要嘛 --self-test")

    entries, xmltree, badging = read_apk(args.apk, args.aapt2)
    results = run_checks(entries, xmltree, badging, APP_ID, IME_SERVICE)
    failed = 0
    for good, msg in results:
        print(("PASS" if good else "FAIL") + ": " + msg)
        if not good:
            failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
