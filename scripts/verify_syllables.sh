#!/usr/bin/env bash
#
# verify_syllables.sh — 九宮格拼音消歧欄的**畫面**驗證
#
# ── 為什麼需要這支腳本 ──────────────────────────────────────────────────
#
#   消歧欄這條線交付時,有的只是「純函式的單元測試 + 人工看的截圖」。
#   而這個專案已經吃過七次「單元測試綠、使用者打開看不到」的虧,
#   使用者原本的回報(cn-t9-pinyin 沒有消歧欄)到今天都沒有重現、原因不明。
#
#   所以這支腳本**只認畫面上的像素**:截圖 → 裁切到消歧欄該在的那一塊 →
#   OCR → 斷言讀得到 `ni` / `mi`。內部狀態對而畫面沒畫出來,它會紅。
#
#   ⚠ 裁切必須夠緊。候選列上的 comment 本來就印著「ni hao」——
#   把候選列一起裁進來,OCR 會讀到 ni 而永遠是綠的。那是最容易做出來的假綠燈。
#
# ── 六道關 ──────────────────────────────────────────────────────────────
#   0  方案漂移:裝置上(APK 裡)的方案必須與 core/data/schemas/ 一致
#   1  範圍非空:掃到的九宮格佈局數必須 >= RS_MIN_T9_LAYOUTS(§2-G)
#   2  第一個音節:打 MG… 之後,消歧欄上讀得到 ni 與 mi
#   3  第二個音節:點下 ni 之後,同一塊區域讀得到 hao / gan / gao 之一
#   4  單音節收斂:打 PGM(= qin 也 = pin)、點 pin 之後,
#      **候選列上不得再有 qin**
#   5  沒被接管的那一格:打 PGM 之後左欄第三格畫的還是原本的標點,
#      **而且點下去宿主輸入框真的多出那個標點**
#
#   每一份佈局開跑前還會再驗一次「裝置上跑的是不是同一份 APK」——
#   見 check_apk_identity。那不是產品的關卡,是**這支腳本自己的**關卡。
#
# ── 第 4 關在守什麼(真機回報的那一條)─────────────────────────────────
#
#   第 2/3 關驗的是「消歧欄換不換音節」,它們**沒有斷言候選有沒有收斂** ——
#   於是有一整類缺陷從它們底下走過去:多音節那一條好好的,單音節那一條
#   點下去完全沒反應,而三份佈局全綠。
#
#   形狀是這樣的:`PGM` 改寫成 `pin` 之後**輸入已經是精確拼音**,
#   `spelling_hints` 沒有提示可給,候選一個 comment 都沒有;而當時的驗收判準
#   問的正是「候選的 comment 有沒有以 pin 開頭」,於是把**最成功**的那一次
#   判成失敗、把輸入串還原。使用者點了 pin,候選一個都沒變。
#
#   所以第 4 關斷言的是**候選列上的 comment**:
#     · 點之前必須讀得到 qin —— **正向對照**,證明這一條真的裁到了候選列、
#       OCR 也讀得出來。少了它,裁歪或 OCR 壞掉都會讀出空字串,
#       而空字串「沒有 qin」—— 又一個假綠燈。
#     · 點之後不准再有 qin。
#
# ── 情境(--scenario):把環境弄壞,產品必須自己不做壞事 ────────────────
#   --scenario stale-schema
#       在裝置上放一份 `t9_pinyin.custom.yaml`,把 `speller/alphabet` 改回
#       舊的單編碼 `'ADGJMPTW'`(= `scripts/collect_data.sh` 沒跑過的機器)。
#       那種方案上 `rs_set_input("niGAM")` **照樣回 true**,引擎卻把 `ni` 當成
#       一段翻不出東西的原文 —— 使用者點第一個候選就上屏「ni好」。
#       斷言:**消歧欄整條不得出現**(IME 啟動時的探針會發現這個方案改寫不了)。
#       正向對照:同一次執行裡候選列必須讀得到 ni(comment 還在),
#       證明 app 真的在組字、OCR 也沒壞 —— 否則「讀不到 ni」會變成假綠燈。
#       ⚠ 這不是 --plant:退出碼**不反轉**。植入的是環境,不是缺陷;
#         要驗的是產品在壞環境裡仍然不做壞事。
#
# ── 第 5 關在守什麼(接線層裡掃原始碼看不到的那一半)───────────────────
#
#   第 2/3/4 關問的都是「**被接管**的那幾格」。而一格被消歧欄接管與否,同時
#   決定四件事(鍵面、點下去做什麼、長按盤開不開、朗讀名念什麼)—— 也就是說
#   消歧欄同時決定了**沒被接管的那一格**會怎樣,而那一格在畫面上完全正常。
#
#   本檔的 --plant tap-swallowed 就是那個形狀,而它在**這一輪之前**是全綠的:
#     · `./gradlew test` 610 條沒有一條會紅 —— T9Syllables.renderSlot 一個字都
#       沒改,純函式全部照舊。
#     · T9SyllablesTest 那條掃原始碼的檢查也不會紅:它當時問的是
#       `grid.contains("slot.tapCell")` 之類,而 KeyGrid 先把回傳值讀進區域變數,
#       於是那幾個 needle **無條件成立**,不管下游拿它們做什麼。
#       (這個植入寫成 `!slotCells.containsKey(key.id)` 的形式,`slotCells[`
#       仍然只出現一次、needle 也仍然都在 —— 舊那一版的每一條斷言它都滿足。)
#     · 第 2/3/4 關驗的每一件事在探針下也都還是好的:讀音畫得出來、點讀音會換
#       一批、候選也收斂。壞掉的只有那一顆沒被接管的標點鍵。
#   而使用者拿到的是:組字途中,左欄那顆「？」按下去什麼都不會發生。
#   ——「畫得對、按下去什麼都不做」正是 task #78 的形狀,換一個判斷點。
#
#   ⚠ 這一輪順手把那條掃原始碼的檢查錨到**整條接線運算式**上了,所以
#     tap-swallowed 現在在 `./gradlew test` 也會紅。**別因此以為這一關多餘。**
#     那一條守的是「這一行長得對」;換一種寫法達成同一件壞事,它就又看不到了。
#     第 5 關問的是「按下去會怎樣」—— 那才是使用者遇到的那一件事。
#
#   所以第 5 關斷言那一格的**行為**:
#     · 畫面:左欄第三格在打字前後**逐像素相同**(它還是佈局畫的那顆標點鍵)。
#       正向對照是前兩格**變了** —— 少了它,裁歪或底圖拍錯都會讓「沒變」
#       無條件成立,又一個假綠燈。
#     · 行為:點下去之後**宿主輸入框真的多出那個標點**。這一條不看 OCR,
#       看的是 uiautomator 讀回來的 EditText 文字 —— 上屏了沒有,問輸入框最準。
#       (實測 cn-t9-pinyin:點下去輸入框從「PGM」變成「親？」。)
#
#   ⚠ 第三格的座標是**從畫面上量出來的**,不是問幾何模型:模型在 cn-t9-pinyin
#     上把 pu_question 放在比實際渲染低約 50px 的地方(numrow 上約 75px,
#     見 ocr_region 的註解)。作法是拿前兩格**變了**的那兩條橫帶的中心外推
#     一格 —— 三個格位在兩份佈局裡都坐在連續三列等高的列上,所以間距就是列距。
#     外推前要求剛好兩條橫帶(PGM = qin/pin 兩個讀音);不是兩條就**指名說出來**,
#     不可以默默少驗一格。
#
#   ⚠ 上方橫排風格的佈局(t9-pinyin)沒有「沒被接管的那一格」—— 用不到的格位
#     根本不畫。那一份會跳過第 5 關,而**跳過與綠燈長得一模一樣**,所以收尾時
#     會斷言第 5 關至少跑到過一次。
#
# ── 沒做:長按盤(而且不打算假裝做了)──────────────────────────────────
#
#   覆核的 P2 植入(KeyGrid 算對了 slot.popup 卻永不開盤)這一支**驗不到**,
#   原因不是 OCR 認不出來,是**驅動不了**。在 emulator-5558 上量過 15 次:
#     · input swipe 同點 700ms / 1200ms、input motionevent DOWN…UP 2s、
#       中間補 MOVE 的、連按兩次的 —— 長按盤總共只開出來 1 次。
#     · 同一台機器、同一次連線,對**宿主 app 的 EditText** 長按每次都叫得出
#       選字工具列(23851 px 的畫面差異),所以不是 adb 注入壞掉。
#     · 按住當中拍的截圖看得到那顆鍵是**按下狀態**,只是 Compose 的
#       detectTapGestures onLongPress 沒有觸發 —— 差別在 IME 視窗這一側。
#   一條 1/15 的關卡比沒有關卡更糟:它會在無關的改動上變紅,而大家會學會忽略它。
#   所以這裡**不做**,並把缺口寫在這裡:
#     · 現在守長按盤的只有 T9SyllablesTest 的「長按盤必須走 slot.popup」與
#       「KeyGrid 不自己決定那一格的行為」—— 那是**掃原始碼**,守得住
#       「有沒有接對線」,守不住「按下去會怎樣」。
#     · 要補的是 Robolectric + compose-ui-test(本模組現在兩個都沒有):
#       setContent { KeyGrid(...) } → performTouchInput { longClick() } →
#       斷言彈出盤出現、而且點它會送出佈局宣告的那個 SubKey。同一層也才守得住
#       覆核的 A2/A3(朗讀名被寫死成 null)—— 那兩個植入本支同樣驗不到,
#       因為朗讀名不在畫面上,在無障礙樹裡,而 uiautomator 看不到 IME 的視窗。
#
# ── 植入違規(證明它會紅)─────────────────────────────────────────────
#   --plant stale-schema   把裝置上的方案換成舊的單編碼版 → 第 0 關必須紅
#   --plant narrow-scope   只掃一份佈局              → 第 1 關必須紅
#   --plant bad-slot-ids   把佈局的 syllable_slots 指到不存在的 key id
#                          → 格位替換不會發生,畫面照常顯示標點 → 第 2 關必須紅
#                          (這正是「有人把 pu_comma 改名」的真實形狀)
#   --plant tap-swallowed  把**沒被接管**的格位的點擊也導進 onSlot(而 onSlot 對
#                          Cell.Original 是 Unit)→ 第 5 關必須紅。
#                          鍵面、幾何、讀音、收斂全部正常,只有那一顆標點鍵按下去
#                          什麼都不會發生。單元測試與掃原始碼的檢查都攔不住它。
#   --plant tap-passthrough 一格的點擊一律走原鍵自己的 onEvent → **被接管**的
#                          那幾格只剩 tap = ActionVerb.NOOP,點讀音沒有反應
#                          → 第 4 關必須紅(點了 pin,候選列/消歧欄上還有 qin)。
#                          ⚠ 覆核把這個植入描述成「鍵面寫著讀音,按下去打出標點」。
#                            實測不是:T9Syllables.slotKey 把被接管那一格的 send
#                            清成 null、tap 設成 NOOP,而 RimeInputMethodService
#                            對 NOOP 是 Unit —— 所以症狀是**點下去什麼都不會發生**,
#                            標點不會上屏。(植入後實跑:點讀音格,輸入框仍是
#                            「PGM」,整個畫面只變了 117 個像素。)
#
#   ⚠ tap-swallowed 與 tap-passthrough 改的是**原始碼**,所以這兩個會自己
#     patch KeyboardView.kt → ./gradlew :app:assembleDebug → 還原檔案 →
#     拿建出來的那份 APK 去驗。還原是 trap 保證的,而且會用 cmp 確認;
#     原本的 app-debug.apk 也會先備份再放回去,免得下一步驗到植入的那一份。
#     它們只跑第一份佈局(驗的是接線,不是佈局),不可以和 --apk 一起用。
#
#   帶 --plant 時**退出碼是反的**:斷言紅了才算通過(exit 0),
#   植入了卻還是綠的就是這支腳本壞了(exit 1)。而且不是「紅就算過」——
#   每一種植入都指名它**應該踩紅哪一條**,踩紅別條(模擬器抽風、裝不上去)
#   一樣算失敗。exit 2 保留給工具/環境問題,不會被反轉。
#
#   前兩種只碰主機上的檔案,不需要裝置,所以第 1 關之後就收尾 ——
#   這樣它們才進得了快車道(見 .github/workflows/build.yml)。
#   bad-slot-ids 斷言的是畫面像素,只能跟著模擬器那條車道跑。
#
#   --check-ci    不跑任何驗證,只檢查上面宣告的每一種 --plant 都真的
#                 **會在目前這條分支上跑**。這一支的三個反向測試曾經
#                 **一次都沒有在 CI 上跑過**(檔頭寫著、workflow 沒接),
#                 而那看起來與一切正常一模一樣。
#
#                 ⚠ 只 grep 「build.yml 裡有沒有這串字」是不夠的 —— 那是這一關
#                   自己踩過的第二次坑:字串接上了,但**分支沒接**。中間隔著
#                   兩道各自獨立、而且都不出聲的閘門:
#                     1. `on: push: branches:` 沒列這條分支 → 推上去整份
#                        workflow 一件都不會觸發;
#                     2. 列了,但那個 --plant 所在的 job 自己的 `if:` 不認這條
#                        分支 → 那個 job 整個被跳過,而**跳過的 job 在 checks
#                        上是灰色的勾**,和跑過而且通過長得一模一樣。
#                   所以這裡除了問「接線在不在」,還會把 build.yml 的
#                   `on: push: branches:` 與 job 的 `if:` 真的算一遍
#                   (scripts/ci_branch_gate.py),任一道不成立就紅,
#                   而且訊息會指名是哪一道擋住的。
#
#                 分支從哪裡來:RIME_CI_BRANCH > GITHUB_HEAD_REF >
#                 GITHUB_REF_NAME > git rev-parse --abbrev-ref HEAD。
#                 問的一律是「**這條分支被 push 的時候**會不會跑」——
#                 在 PR 的 run 上也是問這一件,因為那正是合併前要知道的事。
#
#                 --check-ci --self-test 會**分別**拆掉三樣東西各跑一次:
#                 接線、`on: push: branches:` 裡的這條分支、job 的 `if:` 裡的
#                 這條分支 —— 三次都必須紅,而且必須紅在對應的那一道閘門上
#                 (紅錯地方一樣算失敗:那代表訊息在指錯方向)。
#
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# shellcheck source=lib/product.sh
. "$HERE/lib/product.sh"
# ⚠ 就緒判斷不可以寫成 `logcat | grep -q`:pipefail 之下命中會變成 141
#   (grep -q 一命中就結束 → 上游 SIGPIPE),於是「有命中」被判成「沒命中」。
#   改用 lib/logmatch.sh 的 log_has / log_matches —— 它們先收進變數再用內建比對。
. "$HERE/lib/logmatch.sh"
# ⛔ 裝置選擇的唯一入口。沒有預設 port —— 這台機器上長期有三到四台在跑,
#   而 `adb devices` 以 port 升冪列出,「預設 5554」與「抓第一台」都會
#   落在同一台**別人的**機器上,然後 pm clear 它。
# shellcheck source=lib/device.sh
. "$HERE/lib/device.sh"
# shellcheck source=lib/ocr.sh
. "$HERE/lib/ocr.sh"

SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
ADB="$SDK/platform-tools/adb"
SERIAL=""
IME_ID="${RIME_IME_ID:-$RS_ANDROID_IME_ID}"
IME_PKG="${IME_ID%%/*}"
TARGET_PKG=dev.rime.inputmatrix
TARGET_ACT="$TARGET_PKG/.MainActivity"
THEME="${RIME_THEME:-default-light}"
SCHEMA=t9_pinyin
OUT_DIR="$ROOT/build/verify-syllables"
APK=""
PLANT=""
SCENARIO=""
CHECK_CI=0
SELF_TEST=0
# 每一種植入**指名**它該踩紅的那一條 FAIL 訊息。只看退出碼是不夠的:
# 模擬器沒開、APK 裝不上去一樣是 exit 1,而那不叫「反向測試通過」。
plant_expect_re() {
  case "$1" in
    stale-schema) echo '不一致|alphabet 不含小寫拼音' ;;
    narrow-scope) echo '少於下界' ;;
    bad-slot-ids) echo '消歧欄上讀不到 ni/mi' ;;
    tap-swallowed) echo '按下去什麼都不做的標點鍵' ;;
    tap-passthrough) echo '(候選列|消歧欄)上還有 qin' ;;
    *) echo '' ;;
  esac
}
# 只碰主機檔案、不需要裝置的植入。列在這裡的可以進快車道。
plant_is_host_only() {
  case "$1" in
    stale-schema|narrow-scope) return 0 ;;
    *) return 1 ;;
  esac
}
# 改**原始碼**的植入:要自己 patch → 建 APK → 還原。列在這裡的不可以帶 --apk。
plant_is_source() {
  case "$1" in
    tap-swallowed|tap-passthrough) return 0 ;;
    *) return 1 ;;
  esac
}
KEYBOARD_VIEW="$ROOT/android/app/src/main/java/org/luminakey/ime/keyboard/KeyboardView.kt"
CLEAN_APK_PATH="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"
PLANT_SRC_BACKUP=""
PLANT_APK_BACKUP=""
# 原始碼植入把**裝置上**那一份也換掉了。這裡記著「跑完要把哪一份裝回去」。
DEVICE_RESTORE_APK=""
# ⚠ 還原是 trap 保證的。植入的原始碼留在 worktree 裡,下一個人(或下一條線)
#   會拿它當基準改東西,而 git diff 上看起來就是「有人動了接線」。
restore_planted_tree() {
  if [ -n "$PLANT_SRC_BACKUP" ] && [ -f "$PLANT_SRC_BACKUP" ]; then
    cp "$PLANT_SRC_BACKUP" "$KEYBOARD_VIEW"
    if ! cmp -s "$PLANT_SRC_BACKUP" "$KEYBOARD_VIEW"; then
      echo "!! 還原不了 $KEYBOARD_VIEW —— 請手動 git checkout 它。" >&2
    fi
    PLANT_SRC_BACKUP=""
  fi
  # 建出來的那一份是**植入的** APK,而它就躺在大家慣用的路徑上。放回乾淨的。
  if [ -n "$PLANT_APK_BACKUP" ] && [ -f "$PLANT_APK_BACKUP" ]; then
    cp "$PLANT_APK_BACKUP" "$CLEAN_APK_PATH"
    PLANT_APK_BACKUP=""
  fi
  # 檔案還原了,**裝置上跑的還是植入的那一份**。而慢車道在這一支後面還接著
  # 候選列、匯出/匯入,以及不帶 --apk 的 --scenario —— 它們不會知道這件事,
  # 會驗得好好的,只是驗錯了東西(「驗到別份 APK 卻照樣報結果,比沒測更糟」)。
  if [ -n "$DEVICE_RESTORE_APK" ] && [ -f "$DEVICE_RESTORE_APK" ] && [ -x "$ADB" ]; then
    local back="$DEVICE_RESTORE_APK"
    DEVICE_RESTORE_APK=""
    echo "[syllables] 把裝置上的 $IME_PKG 還原成乾淨的那一份($back)" >&2
    # ⛔ 這一段跑在 EXIT trap 裡,也就是**任何**一條早退路徑都會走到它 ——
    #   包含第 786 行那道閘還沒執行的那些。還原本身是破壞性動作
    #   (uninstall ＋ install ＋ ime set),打錯機器一樣毀掉別條線的那一台。
    #   所以這裡自己再問一次;問不過就不做,並且說出裝置上留著的是哪一份。
    rs_assert_destructive_ok "$ADB" "$SERIAL" "uninstall、install、ime set(還原裝置端 APK)" || {
      echo "!! 沒有指名裝置,略過裝置端還原 —— $IME_PKG 留著的是**植入的**那一份。" >&2
      return
    }
    adbs uninstall "$IME_PKG" >/dev/null 2>&1
    if adbs install -r -g -t "$back" >/dev/null 2>&1; then
      adbs shell ime enable "$IME_ID" >/dev/null 2>&1
      adbs shell ime set "$IME_ID" >/dev/null 2>&1
    else
      echo "!! 還原不了裝置上的 APK —— 下一支腳本若說「裝置上的不是要驗的那一份」,原因在這裡。" >&2
    fi
  fi
}
trap restore_planted_tree EXIT INT TERM

build_planted_apk() {
  local name="$1"
  [ -f "$KEYBOARD_VIEW" ] || { echo "找不到 $KEYBOARD_VIEW" >&2; exit 2; }
  [ -x "$ROOT/android/gradlew" ] || { echo "找不到 $ROOT/android/gradlew" >&2; exit 2; }
  PLANT_SRC_BACKUP="$OUT_DIR/KeyboardView.kt.orig"
  cp "$KEYBOARD_VIEW" "$PLANT_SRC_BACKUP"
  if [ -f "$CLEAN_APK_PATH" ]; then
    PLANT_APK_BACKUP="$OUT_DIR/app-debug.clean.apk"
    cp "$CLEAN_APK_PATH" "$PLANT_APK_BACKUP"
  fi
  # ⚠ 錨點找不到就**當場停**(exit 2)。默默沒植入的話,這一輪會跑完、全綠、
  #   然後被反轉成「這一關沒有在守」—— 一句指著產品的紅字,而壞的是植入。
  python3 - "$name" "$KEYBOARD_VIEW" <<'PLANTPY' || { restore_planted_tree; exit 2; }
import sys

WIRE = """                                onEvent =
                                    if (tapCell == null) onEvent else ({ onSlot(tapCell) }),"""

PLANTS = {
    # 沒被接管的那一格(tapCell == null 而 key.id 在 slotCells 裡)點擊被導進
    # onSlot。其餘的鍵一律不動 —— 這個植入要留下的症狀只有一顆死掉的標點鍵。
    "tap-swallowed": (
        WIRE,
        """                                onEvent =
                                    if (tapCell == null && !slotCells.containsKey(key.id)) onEvent
                                    else ({ onSlot(tapCell ?: T9Syllables.Cell.Original) }),""",
    ),
    "tap-passthrough": (WIRE, "                                onEvent = onEvent,"),
}

name, path = sys.argv[1], sys.argv[2]
old, new = PLANTS[name]
src = open(path, encoding="utf-8").read()
if src.count(old) != 1:
    sys.stderr.write("植入 %s 的錨點在 %s 裡出現 %d 次(要剛好 1 次)——\n"
                     "接線的排版動過了,植入的定義要跟著改。\n" % (name, path, src.count(old)))
    sys.exit(2)
open(path, "w", encoding="utf-8").write(src.replace(old, new))
PLANTPY
  info "已植入 $name,開始建 APK(這一步比較久)"
  if ! ( cd "$ROOT/android" && ./gradlew :app:assembleDebug -q ) \
        >"$OUT_DIR/plant-build.log" 2>&1; then
    echo "植入 $name 之後建不起來,見 $OUT_DIR/plant-build.log" >&2
    tail -20 "$OUT_DIR/plant-build.log" >&2
    restore_planted_tree
    exit 2
  fi
  [ -f "$CLEAN_APK_PATH" ] || { echo "建完了卻找不到 $CLEAN_APK_PATH" >&2; restore_planted_tree; exit 2; }
  cp "$CLEAN_APK_PATH" "$OUT_DIR/planted-$name.apk"
  APK="$OUT_DIR/planted-$name.apk"
  restore_planted_tree
  info "植入的 APK:$APK(原始碼與乾淨的 app-debug.apk 都已還原)"
}
# §2-G:掃描範圍必須非空。三份九宮格佈局(cn-t9-pinyin / -numrow / t9-pinyin)
# 少一份就代表有人刪了佈局、或這裡的判準壞了 —— 兩種都必須紅,而不是靜靜地少驗一份。
MIN_T9_LAYOUTS="${RS_MIN_T9_LAYOUTS:-3}"

# tesseract 可以是系統的,也可以由 RIME_TESSERACT 指定(CI 上用 apt 裝)。
TESSERACT="${RIME_TESSERACT:-$(command -v tesseract || true)}"
TESSDATA="${TESSDATA_PREFIX:-}"

while [ $# -gt 0 ]; do
  case "$1" in
    --serial) SERIAL="$2"; shift 2 ;;
    --apk)    APK="$2"; shift 2 ;;
    --plant)  PLANT="$2"; shift 2 ;;
    --scenario) SCENARIO="$2"; shift 2 ;;
    --theme)  THEME="$2"; shift 2 ;;
    --out)    OUT_DIR="$2"; shift 2 ;;
    --check-ci)  CHECK_CI=1; shift ;;
    --self-test) SELF_TEST=1; shift ;;
    -h|--help)
      # 範圍不要寫死行號:檔頭一長,說明就會被默默截掉一半。
      sed -n '2,/^set -uo pipefail$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知參數: $1" >&2; exit 2 ;;
  esac
done

mkdir -p "$OUT_DIR"
# 打錯的 --plant 名字必須當場停。否則它會被當成「沒有植入」跑完一整輪、
# 全綠、然後被反轉成 exit 1 或(更糟)被當成通過 —— 兩種都在說謊。
if [ -n "$PLANT" ] && [ -z "$(plant_expect_re "$PLANT")" ]; then
  echo "不認得的 --plant「$PLANT」。可用:stale-schema / narrow-scope / bad-slot-ids / tap-swallowed / tap-passthrough" >&2
  exit 2
fi
# 打錯的 --scenario 同理:它會被當成「沒有情境」跑完一整輪正向、全綠,
# 而日誌上看起來與情境真的跑過一模一樣。
if [ -n "$SCENARIO" ] && [ "$SCENARIO" != "stale-schema" ]; then
  echo "不認得的 --scenario「$SCENARIO」。可用:stale-schema" >&2
  exit 2
fi
if [ -n "$SCENARIO" ] && [ -n "$PLANT" ]; then
  echo "--scenario 與 --plant 不能一起用(一個不反轉退出碼、一個反轉)。" >&2
  exit 2
fi
# 原始碼植入自己建 APK。同時給 --apk 的話,兩者一定有一個沒有被用到,
# 而日誌上看起來與正常跑完一模一樣 —— 那正是「驗到別份東西卻照樣報結果」。
if [ -n "$PLANT" ] && plant_is_source "$PLANT" && [ -n "$APK" ]; then
  echo "--plant $PLANT 會自己 patch 原始碼再建一份 APK,不可以同時給 --apk。" >&2
  exit 2
fi
# ⛔ 裝置的選擇**不在這裡**,在底下的「裝置準備」那一段。
#
# 這裡曾經寫著 `rs_select_device ... || exit 2`,而它擋在 `--check-ci` /
# `--plant stale-schema` / `--plant narrow-scope` 這三條**只讀主機檔案**的
# 派發之前。後果是 `.github/workflows/build.yml` 快車道那四次呼叫
# (`--check-ci --self-test`、`--check-ci`、`--plant stale-schema`、
# `--plant narrow-scope`)在 GitHub runner(0 台裝置)上全部 RC=2,
# 訊息是「這台機器上有 0 台裝置在線,不猜」—— 而那四步一台裝置都不需要。
# `publish` 的 `needs:` 掛著 `fast`,於是這一版根本發不出去。
#
# 判準:**要碰裝置的時候才選裝置**。順序見「裝置準備」。
adbs() { "$ADB" -s "$SERIAL" "$@"; }

# ── 引擎準備好了沒:**問**,不要撈歷史日誌(工單 #101)────────────────────
#
# ⛔ 舊寫法是
#       for _ in $(seq 1 40); do
#         log_has "READY" adbs logcat -d -s RimeRuntime:I && { READY=1; break; }
#       done
#   而 `RimeRuntime` 那一行(`Log.i(TAG, "phase → $next")`,見
#   `core/RimeRuntime.kt:282`)**只在相位改變時印一次**。這一輪迴圈在
#   `am start` 之前做 `adbs logcat -c`,於是只要 IME 行程在 `pm clear` 之後、
#   `logcat -c` 之前就已經跑完部署(它是預設輸入法,系統會自己把它拉起來,
#   而上面那個 `ime enable`/`ime set` 迴圈更是直接踩下油門),那一行就被清掉了。
#   IME 行程沒有再被殺過 → 它不會再印 → 這一輪等滿 120 秒 → 紅。
#   而下一輪同一個 commit 原封不動重跑,時序差幾百毫秒就全綠。
#   2026-08-14 發 26081401 時 `cn-t9-pinyin-numrow` 就是這樣紅的,
#   前後兩份佈局全綠 —— 差別只是那兩輪 IME 剛好晚一點才起來。
#
# ✅ 新寫法問的是**現在**:debug 建置的 harness receiver
#   (`<套件名>.devtools.BackupHarnessReceiver`,`--es op state`)
#   收到廣播就把 `RimeRuntime.isReady` / `phase` 印出來。那是一個**答案**,
#   不是一條歷史紀錄:清過 logcat、行程沒重啟、問一百次都答得出來。
#   而且答案裡帶著 phase,所以「還在部署」與「部署失敗」不再長成同一句話。
#
# ⚠ 這個 receiver 只存在於 debug 建置(`src/debug/`),release 沒有,而這支
#   腳本本來就只跑 debug APK(它自己會比對 sha256)。問不到答案時下面會說出
#   「這一份 APK 沒有 harness」這件事,不會冒充成「引擎沒起來」。
# ⚠ 套件名從 $IME_PKG 推,不寫死 —— `verify_product_ids.sh` 第 3 關
#   ("scripts/ 底下沒有寫死的識別碼")會抓。與 verify_backup_roundtrip.sh:83 同一個寫法。
RIME_STATE_RECEIVER="$IME_PKG/$IME_PKG.devtools.BackupHarnessReceiver"

# 問一次。印出 `ready=… phase=… …` 那一行(問不到就什麼都不印,RC=1)。
rime_state_once() {
  local nonce out tail
  nonce="rs$(date +%s%N)"
  adbs shell am broadcast -n "$RIME_STATE_RECEIVER" --es op state --es path "$nonce" \
    >/dev/null 2>&1
  local i
  for i in $(seq 1 20); do
    out="$(adbs logcat -d -s BACKUPRT 2>/dev/null || true)"
    # 只看**這一次**提問之後印出來的東西:nonce 是我們自己帶進去的,
    # 它出現在 `begin op=state path=<nonce>` 那一行。
    case "$out" in *"$nonce"*) tail="${out##*"$nonce"}" ;; *) tail="" ;; esac
    case "$tail" in
      *"ready="*)
        printf '%s\n' "$tail" | sed -n 's/.*\(ready=.*\)$/\1/p' | head -1
        return 0 ;;
    esac
    sleep 0.5
  done
  return 1
}

# 等到 ready=true(或超時)。RC=0 = 好了;RC=1 = 沒好,理由印在 stdout。
rime_wait_ready() {
  local deadline=$((SECONDS + ${1:-120})) line=""
  while [ "$SECONDS" -lt "$deadline" ]; do
    line="$(rime_state_once || true)"
    case "$line" in
      *"ready=true"*) return 0 ;;
      "") ;;                    # 沒人接廣播 —— 下面會分辨這一種
    esac
    sleep 2
  done
  printf '%s' "${line:-<harness 沒有回應>}"
  return 1
}
info() { echo "[syllables] $*" >&2; }
pass() { echo "  [PASS] $*"; }
FAILURES=0
FAIL_LOG=""
# 第 5 關真的跑到過幾次。**跳過的關卡與綠燈長得一模一樣**,而第 5 關只在
# 左側直欄那種佈局上成立 —— 判準壞掉時它會一份都跑不到,而且不會有任何徵狀。
GATE5_RAN=0
# ⚠ 訊息要留下來。「紅了」不等於「該紅的那一條紅了」:模擬器抽風、APK 裝不上去
#   同樣會讓退出碼變 1,而 --plant 的斷言若只看退出碼,就會把環境故障當成
#   「反向測試通過」—— 那正是這一支要防的假綠燈的另一個形狀。
fail() { echo "  [FAIL] $*" >&2; FAILURES=$((FAILURES + 1)); FAIL_LOG="$FAIL_LOG
$*"; }

# 收尾。帶 --plant 時退出碼是**反的**,而且要求紅的是指名的那一條。
finish() {
  echo
  if [ -n "$PLANT" ]; then
    local want; want="$(plant_expect_re "$PLANT")"
    if [ "$FAILURES" -eq 0 ]; then
      echo "✗ 植入了 $PLANT,斷言卻還是全綠 —— 這一關沒有在守。artifact:$OUT_DIR"
      return 1
    fi
    if ! printf '%s' "$FAIL_LOG" | grep -qE "$want"; then
      echo "✗ 植入了 $PLANT,紅的卻不是該紅的那一條(要找的是 /$want/)。"
      echo "  實際紅的是:"
      printf '%s\n' "$FAIL_LOG" | sed '/^$/d; s/^/    /'
      echo "  artifact:$OUT_DIR"
      return 1
    fi
    echo "✓ 反向測試通過:植入 $PLANT 之後,該紅的那一條紅了(共 $FAILURES 項)"
    echo "   artifact:$OUT_DIR"
    return 0
  fi
  if [ "$FAILURES" -gt 0 ]; then
    echo "✗ $FAILURES 項沒過。artifact:$OUT_DIR"
    return 1
  fi
  echo "✓ $1"
  echo "   artifact:$OUT_DIR"
  return 0
}

step() { echo; echo "── $* ──"; }

# IME 視窗的 frame,以及由它回推的格線區頂端。
#
# ⚠ **每次截圖與每次點擊之前都要重讀。** 上方橫排一出現,IME 視窗就往上長,
#   frame_top 會變小 —— 拿打字前讀到的值去裁,裁到的正好是候選列,而候選列上的
#   comment 本來就印著「ni hao」,於是 OCR 讀得到 ni,測試永遠綠。
#   那是這支腳本最容易做出來的假綠燈(第一版就是這樣,實測抓到)。
#
# ⛔ **格線區的頂端由上往下算,不從視窗底端回推。**
#
# 這裡從前寫的是 `GRID_TOP=$((FRAME_BOT - GRID_H))`,而 IME 視窗的下緣是螢幕
# 下緣、鍵盤內容卻讓出了一段 `honor_bottom_inset`(手勢列)。實測 emulator-5558:
# 視窗 frame 高 800 px、bar 118 px、格線區 619 px,兩條公式差
# **63 px**(= 800 − 118 − 619,正是那段 inset)。九宮格的鍵有 123 px 高,
# 低 63 px 剛好還壓在同一顆鍵的下緣 —— 所以它一直沒有紅過;底列那一排就沒這麼
# 好運。這支腳本內部另外長出了兩段補償碼(`:1170` 與 `:1338`)就是為了繞過它。
#
# 由上往下算沒有這個問題:候選列緊貼視窗頂端,格線區緊貼候選列。這與
# `verify_candbar.sh` / `verify_layout.sh` 現在用的是同一條公式 —— 三支腳本
# 從前各自主張對方是錯的,這一輪收斂成一份。
#
# ⚠ 但也不可以寫成 `FRAME_TOP + BAR_PX`:上方橫排(消歧欄畫在候選列上方時)
#   會讓視窗往上長一整排,而那一排在 `bar_height_px` 之外。
#
# 所以:**先在還沒打字的狀態量一次底部 inset**(視窗高 − 候選列 − 格線區),
# 之後一律 `GRID_TOP = FRAME_BOT − inset − GRID_H`。上方多不多一排都不影響它,
# 而它與 `verify_candbar.sh` / `verify_layout.sh` 的 `FRAME_TOP + BAR_H` 在
# 沒有上方橫排時**逐 px 相同** —— 三支腳本從前各自主張對方是錯的,收斂成一份。
read_frame() {
  read -r FRAME_TOP FRAME_BOT < <(adbs shell dumpsys window windows 2>/dev/null | tr -d '\r' | python3 -c '
import sys, re
txt = sys.stdin.read()
m = re.search(r"Window\{[^}]*InputMethod\}:(.*?)(?=\n  Window \#|\Z)", txt, re.S)
if not m: sys.exit(0)
f = re.search(r"\bframe=\[(\d+),(\d+)\]\[(\d+),(\d+)\]", m.group(1))
if f: print(f.group(2), f.group(4))
')
  [ -n "${FRAME_BOT:-}" ] || return 1
  if [ -z "${BASE_INSET:-}" ]; then
    # 第一次(還沒開始打字,上方橫排還沒出現)量出底部 inset:
    #   視窗高 = 候選列 ＋ 格線區 ＋ inset
    BASE_INSET=$(( (FRAME_BOT - FRAME_TOP) - BAR_PX - GRID_H ))
    if [ "$BASE_INSET" -lt -8 ] || [ "$BASE_INSET" -gt 220 ]; then
      echo "格線區幾何對不上:視窗 $FRAME_TOP..$FRAME_BOT、bar $BAR_PX、格線 $GRID_H" >&2
      echo "  推得 honor_bottom_inset = $BASE_INSET px(合理範圍 0..220)—— 中止" >&2
      echo "  拿一組錯的座標去點,症狀會是「點在隔壁列」而報告會說產品壞了。" >&2
      BASE_INSET=""
      return 1
    fi
    echo "[syllables] 底部 inset = $BASE_INSET px(視窗 $((FRAME_BOT - FRAME_TOP))、bar $BAR_PX、格線 $GRID_H)" >&2
  fi
  GRID_TOP=$((FRAME_BOT - BASE_INSET - GRID_H))
  return 0
}

# 裝置與 OCR 的工具檢查。**依然不准跳過**,只是挪到真的要碰裝置之前才問 ——
# 第 0/1 關純粹讀主機上的檔案,不需要 adb 也不需要 tesseract,而
# stale-schema / narrow-scope 兩個植入只驗那兩關。要求它們先有一台模擬器,
# 等於把這兩個反向測試永遠關在慢車道外面(它們至今一次都沒跑過)。
require_device_tools() {
  [ -x "$ADB" ] || { echo "找不到 adb:$ADB" >&2; exit 2; }
  # ⚠ 不可以「找不到 OCR 就跳過」。跳過的關卡與綠燈長得一模一樣 ——
  #   而這一關**整輪就是這樣沒跑過的**:掃描階段綠,然後 exit 2,
  #   `build/verify-syllables/` 留下一個空目錄,沒有人發現。
  #   `rs_find_tesseract` 會連解包安裝的那一份一起找,並且自證跑得動。
  rs_find_tesseract || exit 2
  TESSERACT="$RS_TESSERACT"
  info "OCR:$TESSERACT(tessdata=${TESSDATA_PREFIX:-<預設>})"
  # ⚠ 同樣的道理,但這一條是**吃過的虧**:GitHub runner 上沒有 Pillow,
  #   裁切那段 python 每次都 ModuleNotFoundError,而腳本沒有 -e、
  #   `ocr_region` 的輸出檔就是空的 —— 於是三份佈局都報
  #   「消歧欄上讀不到 ni/mi」。**看起來像產品壞了,其實是關卡自己缺套件。**
  #   工具缺席必須在這裡就停,而且訊息要指向安裝,不要指向產品。
  if ! python3 -c "import PIL" >/dev/null 2>&1; then
    echo "python3 缺 Pillow(裁切要用)。請安裝:pip3 install --user pillow" >&2
    exit 2
  fi
}

# ═══════════ --check-ci:反向測試會不會在**這條分支**上真的跑 ═══════════
#
# 這一關擋的是這支腳本自己踩過兩次的坑:
#   第一次 —— 檔頭把三種 --plant 寫得清清楚楚,workflow 卻一次都沒有呼叫過。
#   第二次 —— workflow 呼叫了,但**這條分支既不在 `on: push: branches:` 裡,
#             也不在那個 job 的 `if:` 裡**,於是推上去以後:整份 workflow 不觸發
#             (第一道),或那個 job 被跳過(第二道)。而**被跳過的 job 在
#             checks 上是灰色的勾**,和跑過而且通過長得一模一樣。
# 兩次都是「宣告了」與「在跑」在日誌上分不出來。所以這裡問的不是
# 「build.yml 裡有沒有這串字」,是「**推這條分支上去,那一步會不會執行**」。
if [ "$CHECK_CI" -eq 1 ]; then
  WF_REAL="$ROOT/.github/workflows/build.yml"
  SELF="${BASH_SOURCE[0]}"
  GATE="$HERE/ci_branch_gate.py"
  [ -f "$WF_REAL" ] || { echo "找不到 $WF_REAL" >&2; exit 2; }
  [ -f "$GATE" ] || { echo "找不到 $GATE(閘門判讀在那裡)" >&2; exit 2; }

  # 分支名。CI 上 GITHUB_REF_NAME 在 pull_request 事件是「123/merge」,
  # 那不是分支 —— 所以 PR 上優先看 GITHUB_HEAD_REF(來源分支),
  # 問的一律是「這條分支被 push 的時候會不會跑」。
  CI_BRANCH="${RIME_CI_BRANCH:-${GITHUB_HEAD_REF:-${GITHUB_REF_NAME:-}}}"
  if [ -z "$CI_BRANCH" ]; then
    CI_BRANCH="$(git -C "$ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || true)"
  fi
  if [ -z "$CI_BRANCH" ] || [ "$CI_BRANCH" = "HEAD" ]; then
    echo "問不出目前的分支名(HEAD 是 detached?)。請給 RIME_CI_BRANCH=<分支>。" >&2
    exit 2
  fi

  # 宣告的植入種類直接從檔頭讀,不要在這裡再抄一份 —— 抄的那一份會漂移,
  # 而漂移時「檢查通過」的那一份說了算。
  mapfile -t DECLARED < <(sed -n 's/^#   --plant \([a-z-][a-z-]*\).*/\1/p' "$SELF" | sort -u)
  if [ "${#DECLARED[@]}" -lt 5 ]; then
    echo "!! 檔頭只解析出 ${#DECLARED[@]} 種 --plant —— 解析式壞了,這一關在空轉。" >&2
    exit 1
  fi
  mapfile -t SCENARIOS < <(sed -n 's/^#   --scenario \([a-z-][a-z-]*\).*/\1/p' "$SELF" | sort -u)
  if [ "${#SCENARIOS[@]}" -lt 1 ]; then
    echo "!! 檔頭一個 --scenario 都沒解析出來 —— 解析式壞了,這一關在空轉。" >&2
    exit 1
  fi
  info "檔頭宣告的植入:${DECLARED[*]};情境:${SCENARIOS[*]};分支:$CI_BRANCH"

  # 每一根「針」都是一整條命令,不是光禿禿的 --plant X:光有 --plant X
  # 連「是哪一支腳本被呼叫」都不保證。註解裡的字串不算(ci_branch_gate.py 會濾掉)。
  NEEDLES=()
  for sc in "${SCENARIOS[@]}"; do NEEDLES+=("verify_syllables.sh --scenario $sc"); done
  for pl in "${DECLARED[@]}"; do NEEDLES+=("verify_syllables.sh --plant $pl"); done
  # 正向那一次也要在,否則把正向拿掉、只留植入,一樣是全綠。
  NEEDLES+=("verify_syllables.sh --apk")

  # 對一份 build.yml 問一次。回傳:0 = 都會跑,1 = 有閘門擋著,2 = 判斷不了。
  ask_gate() {
    local wf="$1" br="$2" out="$3" n args=()
    for n in "${NEEDLES[@]}"; do args+=(--needle "$n"); done
    python3 "$GATE" --workflow "$wf" --branch "$br" "${args[@]}" >"$out" 2>&1
  }

  if [ "$SELF_TEST" -eq 1 ]; then
    # ── 反向測試的反向測試 ────────────────────────────────────────────
    # 三樣東西**各拆一次**,每一次都必須紅,而且必須紅在對應的那一道閘門上。
    # 「紅了」不等於「該紅的那一條紅了」——只看退出碼的話,一個把每份 build.yml
    # 都判成紅的壞掉版本會全數通過。
    SELF_DIR="$(mktemp -d)"
    NOSUCH="__self_test_no_such_branch__"
    ST_FAILED=0
    st_case() {  # $1 = 說明,$2 = 變異後的 wf,$3 = 必須出現的標記
      local why="$1" wf="$2" want="$3" rc out="$SELF_DIR/out.txt"
      ask_gate "$wf" "$CI_BRANCH" "$out"; rc=$?
      if [ "$rc" -eq 0 ]; then
        echo "  [FAIL] 自我測試「$why」:拆掉了卻還是綠的。" >&2
        ST_FAILED=$((ST_FAILED + 1)); return
      fi
      # ⚠ -F:標記長成 FAIL[job-if],`[…]` 在正規式裡是字元集 ——
      #   不加 -F 的話 grep 找的是「FAIL 後面接一個 j/o/b/-/i/f」,永遠不會命中,
      #   於是每一條自我測試都會報「訊息在指錯方向」。
      if ! grep -qF -- "$want" "$out"; then
        echo "  [FAIL] 自我測試「$why」:紅了,但紅的不是 $want —— 訊息在指錯方向:" >&2
        sed 's/^/         /' "$out" >&2
        ST_FAILED=$((ST_FAILED + 1)); return
      fi
      echo "  [PASS] 自我測試「$why」→ $want"
      grep -m1 -F -- "$want" "$out" | sed 's/^/         /'
    }

    # (甲)接線被拆掉一條。
    WF_A="$SELF_DIR/a.yml"
    grep -v -- "--plant narrow-scope" "$WF_REAL" > "$WF_A"
    st_case "拆掉 narrow-scope 的接線" "$WF_A" "FAIL[not-wired]"

    # (乙)`on: push: branches:` 裡的這條分支被拿掉(整份 workflow 不觸發)。
    #      只動清單項那一行,不動 if: 裡的 refs/heads/…,這樣兩道才分得開。
    WF_B="$SELF_DIR/b.yml"
    sed -E "s|^([[:space:]]*)- ${CI_BRANCH}\$|\1- ${NOSUCH}|" "$WF_REAL" > "$WF_B"
    if ! grep -q -- "- $NOSUCH" "$WF_B"; then
      echo "  [FAIL] 自我測試備不出「分支不在 branches: 裡」那一份 —— " \
           "build.yml 裡找不到「- $CI_BRANCH」這一行。" >&2
      ST_FAILED=$((ST_FAILED + 1))
    else
      st_case "把 $CI_BRANCH 從 on: push: branches: 拿掉" "$WF_B" "FAIL[push-branches]"
    fi

    # (丙)job 的 if: 裡的這條分支被拿掉(job 被跳過 = 灰色的勾)。
    #      用改名而不是刪行:刪掉的若是最後一項,括號會不對稱,那就變成
    #      「YAML 壞了」而不是「這條分支不在 if: 裡」——驗到的會是別件事。
    WF_C="$SELF_DIR/c.yml"
    sed "s|refs/heads/${CI_BRANCH}'|refs/heads/${NOSUCH}'|g" "$WF_REAL" > "$WF_C"
    if ! grep -q -- "refs/heads/$NOSUCH" "$WF_C"; then
      echo "  [FAIL] 自我測試備不出「分支不在 job 的 if: 裡」那一份 —— " \
           "build.yml 的 if: 裡找不到 refs/heads/$CI_BRANCH。" >&2
      ST_FAILED=$((ST_FAILED + 1))
    else
      st_case "把 $CI_BRANCH 從慢車道 job 的 if: 拿掉" "$WF_C" "FAIL[job-if]"
    fi

    # (丁)沒動過的那一份必須是綠的。少了這一條,一個「永遠說紅」的版本
    #      會把上面三條全數通過。
    ST_OUT="$SELF_DIR/clean.txt"
    if ask_gate "$WF_REAL" "$CI_BRANCH" "$ST_OUT"; then
      echo "  [PASS] 自我測試「沒動過的 build.yml」→ 綠"
    else
      echo "  [FAIL] 自我測試「沒動過的 build.yml」:應該是綠的,卻紅了:" >&2
      sed 's/^/         /' "$ST_OUT" >&2
      ST_FAILED=$((ST_FAILED + 1))
    fi

    # 閘門判讀本身也有自己的一套(運算式直譯器、註解不算接線……)。
    if python3 "$GATE" --self-test > "$SELF_DIR/gate.txt" 2>&1; then
      echo "  [PASS] ci_branch_gate.py --self-test($(grep -c '^✓' "$SELF_DIR/gate.txt") 條)"
    else
      echo "  [FAIL] ci_branch_gate.py --self-test 沒過:" >&2
      sed 's/^/         /' "$SELF_DIR/gate.txt" >&2
      ST_FAILED=$((ST_FAILED + 1))
    fi

    rm -rf "$SELF_DIR"
    echo
    if [ "$ST_FAILED" -gt 0 ]; then
      echo "✗ 自我測試失敗 $ST_FAILED 條 —— --check-ci 這一關本身不可信。"
      exit 1
    fi
    echo "✓ 自我測試通過:三道閘門(接線 / push 的 branches / job 的 if)各拆一次都會紅,"
    echo "  沒動過的那一份是綠的,而且每一次紅的都是對應的那一道。"
    exit 0
  fi

  GATE_OUT="$(mktemp)"
  ask_gate "$WF_REAL" "$CI_BRANCH" "$GATE_OUT"; GATE_RC=$?
  while IFS= read -r line; do
    case "$line" in
      PASS*) pass "${line#PASS }" ;;
      FAIL*) fail "$line" ;;
      *)     echo "         $line" ;;
    esac
  done < "$GATE_OUT"
  rm -f "$GATE_OUT"
  echo
  if [ "$GATE_RC" -eq 2 ]; then
    echo "✗ 閘門判讀不了(見上面)。**判斷不了不可以當成綠** —— 這一關就是為了"
    echo "  不讓「看起來接上了」代替「真的會跑」。"
    exit 2
  fi
  if [ "$GATE_RC" -ne 0 ]; then
    echo "✗ 在分支「$CI_BRANCH」上,有反向測試不會跑(上面指名了是哪一道閘門)。"
    echo "  要修的通常是這兩個地方之一:"
    echo "    · .github/workflows/build.yml 的 on: push: branches:(加一項到既有那一行的清單裡,"
    echo "      **不要新增第二個 branches: 鍵**);"
    echo "    · 慢車道 job 的 if:(加一條 github.ref == 'refs/heads/$CI_BRANCH')。"
    exit 1
  fi
  echo "✓ 檔頭宣告的 ${#DECLARED[@]} 種植入與 ${#SCENARIOS[@]} 種情境都接進 build.yml 了,"
  echo "  正向那一次也在,而且推「$CI_BRANCH」上去時每一步所在的 job 都真的會執行。"
  exit 0
fi

# ═══════════════════════ 第 0 關:方案漂移 ═══════════════════════
#
# core/data/shared/ 是 scripts/collect_data.sh 產生的,而且在 .gitignore 裡。
# 協調端改了 core/data/schemas/ 之後,沒重跑 collect_data.sh 的機器上,
# 裝置拿到的仍是**舊方案** —— 症狀是 rs_set_input() 回 false,
# 看起來像前端寫壞了。這一關就是為了不讓下一個人再查一次那件事。
step "0. 方案漂移"
SRC_SCHEMA="$ROOT/core/data/schemas/$SCHEMA.schema.yaml"
SHIPPED_SCHEMA="$ROOT/core/data/shared/$SCHEMA.schema.yaml"
[ -f "$SRC_SCHEMA" ] || { echo "找不到 $SRC_SCHEMA" >&2; exit 2; }

if [ "$PLANT" = "stale-schema" ]; then
  info "植入違規:把 shared 的方案換成舊的單編碼版"
  mkdir -p "$(dirname "$SHIPPED_SCHEMA")"
  sed -e "s/^  alphabet: .*/  alphabet: 'ADGJMPTW'/" \
      -e "s/^  spelling_hints: .*/  spelling_hints: 5/" "$SRC_SCHEMA" > "$OUT_DIR/planted.schema.yaml"
  SHIPPED_SCHEMA="$OUT_DIR/planted.schema.yaml"
fi

if [ ! -f "$SHIPPED_SCHEMA" ]; then
  fail "core/data/shared/$SCHEMA.schema.yaml 不存在 —— 先跑 scripts/collect_data.sh。"
elif ! cmp -s "$SRC_SCHEMA" "$SHIPPED_SCHEMA"; then
  fail "裝置要裝的方案與 core/data/schemas/ 不一致(shared 是產生檔,且在 gitignore 裡)。"
  fail "  先跑 scripts/collect_data.sh;跑不動時最少要把這一個檔案複製過去:"
  fail "  cp core/data/schemas/$SCHEMA.schema.yaml core/data/shared/"
  diff <(grep -E '^  (alphabet|spelling_hints):' "$SRC_SCHEMA") \
       <(grep -E '^  (alphabet|spelling_hints):' "$SHIPPED_SCHEMA") >&2 || true
else
  pass "core/data/shared/$SCHEMA.schema.yaml 與 schemas/ 一致"
fi

# 雙編碼是消歧欄的前提:沒有小寫拼音,rs_set_input("niGAM") 一定被拒。
if grep -qE "^  alphabet: '.*[a-z].*'" "$SHIPPED_SCHEMA" 2>/dev/null; then
  pass "方案是雙編碼(alphabet 含小寫拼音)"
else
  fail "方案的 alphabet 不含小寫拼音 —— 音節改寫一定失敗(rs_set_input 回 false)"
fi

# ═══════════════════════ 第 1 關:掃描範圍 ═══════════════════════
step "1. 掃描範圍(§2-G:範圍必須非空)"
mapfile -t T9_LAYOUTS < <(
  cd "$ROOT/core/layouts" || exit 1
  for f in *.yaml; do
    grep -qE "^for_schema:.*\"$SCHEMA\"" "$f" && basename "$f" .yaml
  done | sort
)
if [ "$PLANT" = "narrow-scope" ]; then
  info "植入違規:只掃一份佈局"
  T9_LAYOUTS=("${T9_LAYOUTS[0]}")
fi
info "宣告 for_schema 含 $SCHEMA 的佈局:${T9_LAYOUTS[*]:-<空>}"
if [ "${#T9_LAYOUTS[@]}" -lt "$MIN_T9_LAYOUTS" ]; then
  fail "只掃到 ${#T9_LAYOUTS[@]} 份九宮格佈局,少於下界 $MIN_T9_LAYOUTS。"
  fail "  這一關擋的是「判準壞了 → 掃到 0 份 → 全綠」。"
else
  pass "掃到 ${#T9_LAYOUTS[@]} 份九宮格佈局(下界 $MIN_T9_LAYOUTS)"
fi

# 掃不到就沒有東西可驗了,直接收尾(前面已經記了 FAIL)。
[ "${#T9_LAYOUTS[@]}" -gt 0 ] || { echo; echo "✗ 沒有可驗的佈局"; exit 1; }

# 情境問的是「引擎接不了精確拼音的時候,產品做什麼」—— 那是**服務層**的判斷,
# 與佈局無關(三份佈局共用同一個 IME service)。跑一份就夠,跑三份只是把
# 慢車道再拉長三倍。第 2/3/4 關才需要每一份都跑,因為那幾關驗的是畫面。
if [ -n "$SCENARIO" ]; then
  info "情境 $SCENARIO:只跑第一份佈局(${T9_LAYOUTS[0]}) —— 驗的是服務層,不是佈局"
  T9_LAYOUTS=("${T9_LAYOUTS[0]}")
fi

# 原始碼植入驗的是 KeyGrid 的接線,三份佈局共用同一段程式碼。跑一份就夠,
# 而且它前面還多一次 Gradle 建置 —— 跑三份只是把慢車道再拉長三倍。
if [ -n "$PLANT" ] && plant_is_source "$PLANT"; then
  info "$PLANT 是原始碼植入:只跑第一份佈局(${T9_LAYOUTS[0]}) —— 驗的是接線,不是佈局"
  T9_LAYOUTS=("${T9_LAYOUTS[0]}")
fi

# 主機端的植入到這裡就驗完了 —— 第 0/1 關只讀檔案。再往下開模擬器
# 證明不了多一件事,卻會讓這兩個反向測試永遠上不了快車道。
if [ -n "$PLANT" ] && plant_is_host_only "$PLANT"; then
  info "$PLANT 只驗主機端的第 0/1 關,不需要裝置 —— 到此收尾。"
  finish ""; exit $?
fi

# ═══════════════════════ 裝置準備 ═══════════════════════
# ⛔ `--serial` 也要算「指名」。閘從前只看環境變數,於是這一行帶 `--serial`
#   就必死(RC=2,訊息說「是自動選來的」而那台正是命令列指名的)。
#   `rs_select_device` 把來源(flag / env / auto)一起記下來給閘看。
#
# ⚠ 這一段**必須留在 host-only 派發之後**。搬到前面去,快車道那四次
#   不需要裝置的呼叫就會在 0 裝置的 runner 上全部 RC=2。
rs_select_device "$ADB" "$SERIAL" || exit 2
SERIAL="$RS_SERIAL"
rs_assert_destructive_ok "$ADB" "$SERIAL" "pm clear、uninstall、ime set" || exit 2
# 原始碼植入的 APK 在這裡才建:第 0/1 關只讀主機上的檔案,先跑完它們,
# 參數打錯、佈局掃不到的時候就不必白等一次 Gradle。
if [ -n "$PLANT" ] && plant_is_source "$PLANT"; then
  build_planted_apk "$PLANT"
fi
require_device_tools

# 裝一份 APK 上去。
#
# ⚠ adb 的原話一定要留下來。這裡以前是
#     `adbs install … >/dev/null 2>&1 || { echo "安裝失敗"; exit 2; }`
#   於是 CI 上只剩「安裝失敗」四個字 —— 簽章不合、版本降級、空間不足在日誌上
#   長得一模一樣,而這一支跑在模擬器車道上,現場不會留到下一次。
#
# ⚠ 原始碼植入建出來的那一份,**與裝置上那一份不見得是同一把金鑰簽的**:
#   慢車道沒有「還原簽章環境」那一步(那是快車道的),所以 runner 上的
#   assembleDebug 會退回 Android 預設的 debug 金鑰,而裝置上跑的是快車道用
#   正式金鑰簽的那一份 → INSTALL_FAILED_UPDATE_INCOMPATIBLE。版本號同理
#   (`rime.versionCode` 也只在快車道寫進 ~/.gradle)。
#   這兩種都不是產品缺陷,是「同一支 app 的兩份建置」的必然結果 ——
#   所以撞到它們就先解除安裝再裝一次,而且**把原因說出來**,不要靜靜重試:
#   靜靜重試會把「使用者升級時真的裝不上去」也一起吞掉,而那是承重的一條。
install_apk() {
  local apk="$1" out rc
  info "安裝 $apk"
  out="$(adbs install -r -g -t "$apk" 2>&1)"; rc=$?
  if [ "$rc" -eq 0 ] && ! printf '%s' "$out" | grep -qi "failure"; then
    return 0
  fi
  info "安裝沒成功,adb 說:$(printf '%s' "$out" | tr '\n' ' ')"
  case "$out" in
    *UPDATE_INCOMPATIBLE*|*INCONSISTENT_CERTIFICATES*|*VERSION_DOWNGRADE*|*signatures*)
      info "→ 這是同一支 app 的兩份建置(金鑰或版本號不同),不是產品問題:先解除安裝再裝一次。"
      adbs uninstall "$IME_PKG" >/dev/null 2>&1
      out="$(adbs install -r -g -t "$apk" 2>&1)"; rc=$?
      if [ "$rc" -eq 0 ] && ! printf '%s' "$out" | grep -qi "failure"; then
        info "→ 解除安裝之後裝上去了。"
        return 0
      fi
      ;;
  esac
  echo "安裝 $apk 失敗(exit $rc):$(printf '%s' "$out" | tr '\n' ' ')" >&2
  exit 2
}

if [ -n "$APK" ]; then
  install_apk "$APK"
  # 裝上去的是植入的那一份 → 這一輪結束時要把乾淨的那一份裝回裝置(見
  # restore_planted_tree)。CLEAN_APK_PATH 在 build_planted_apk 收尾時
  # 已經被還原成乾淨的那一份了。
  if [ -n "$PLANT" ] && plant_is_source "$PLANT" && [ -f "$CLEAN_APK_PATH" ]; then
    DEVICE_RESTORE_APK="$CLEAN_APK_PATH"
  fi
fi
# ⚠ 見 verify_candbar.sh 同一行的註解:不帶 --apk 時要靠 `pm path` ＋
#   裝置端 sha256 記下「這一輪量的是哪一份」。
# ⚠ **裝完才寫**(eb3c588 的題旨)。寫在安裝之前的話,`pkg_apk_sha256`
#   記的是上一次裝的那一份。
rs_write_device_stamp "$ADB" "$SERIAL" "$OUT_DIR/device.txt" "${APK:-}" "$IME_PKG"

# ── 裝置上跑的是不是同一份 APK ────────────────────────────────────────────
#
# ⚠ 這一關是**這一輪真的吃到的虧**。emulator-5554 是四條線共用的,而別條線
#   也在對它 `adb install`。實際發生的事:我裝上去之後大約一分鐘,另一條線
#   (rime-fix5-preedit)把它自己的 build 蓋了上來,於是接下來三份佈局驗的
#   全是**別人的 APK** —— 新加的第 4 關三份佈局一致地紅,看起來像修正沒生效,
#   查到 logcat 才發現裝置上跑的根本是舊的那一份。
#
#   「驗到別份東西卻照樣報結果」與這支腳本第 2 關那句
#   「裝置上載入的卻是 $ACTIVE」是同一種錯誤,只是換了一層。
#
# 判準用 sha256:檔案大小會撞、版本號四條線都一樣、`pm path` 只給得出路徑。
# 沒帶 --apk 時就把「開跑當下裝置上那一份」當成基準 —— 這一支要求的是
# **從頭到尾同一份**,至於它是誰建的由呼叫端負責。
device_apk_sha() {
  local p
  p="$(adbs shell pm path "$IME_PKG" 2>/dev/null | tr -d '\r' | sed -n 's/^package://p' | head -1)"
  [ -n "$p" ] || return 1
  adbs shell "sha256sum '$p'" 2>/dev/null | tr -d '\r' | awk '{print $1}'
}
APK_SHA=""
if [ -n "$APK" ]; then
  APK_SHA="$(sha256sum "$APK" | awk '{print $1}')"
else
  APK_SHA="$(device_apk_sha || echo)"
fi
if [ -z "$APK_SHA" ]; then
  echo "讀不到 APK 的 sha256(本機或裝置)—— 無從確認驗的是哪一份,不能往下跑。" >&2
  exit 2
fi
info "要驗的 APK sha256=${APK_SHA:0:12}…"

check_apk_identity() {
  local got; got="$(device_apk_sha || echo)"
  [ "$got" = "$APK_SHA" ] && return 0
  fail "$1:裝置上的 $IME_PKG 不是要驗的那一份 APK(裝置 ${got:-<讀不到>} ≠ $APK_SHA)。"
  fail "  emulator 是共用的 —— 多半是別條線在同一台上 adb install 了它自己的 build。"
  fail "  驗到別份 APK 卻照樣報結果,比沒測更糟,所以這裡直接停。"
  return 1
}

# 每一份佈局跑一輪
for LAYOUT in "${T9_LAYOUTS[@]}"; do
  step "2/3. $LAYOUT"
  LOUT="$OUT_DIR/$LAYOUT"
  mkdir -p "$LOUT"

  # ── 釘方案與佈局 ────────────────────────────────────────────────
  # ⚠ pm clear 會把我們踢出「已啟用的輸入法」,系統當場退回 Gboard;
  #   而 force-stop 待測 IME 會把套件打進 stopped 狀態、**再也回不來**。
  #   所以:只用 pm clear(它本來就會殺掉行程),而且事後一定要確認預設輸入法。
  adbs shell pm clear "$IME_PKG" >/dev/null 2>&1
  adbs shell pm clear "$TARGET_PKG" >/dev/null 2>&1
  sleep 3

  adbs shell "run-as $IME_PKG mkdir -p shared_prefs" >/dev/null 2>&1
  printf '%s' "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <string name=\"pending_schema\">$SCHEMA</string>
</map>
" | adbs shell "run-as $IME_PKG sh -c 'cat > shared_prefs/$RS_ANDROID_PREFS_STORE.xml'" >/dev/null 2>&1

  # ── 情境 stale-schema:把裝置上的方案改回舊的單編碼版 ───────────────
  # 用 `<schema>.custom.yaml` 的 patch,不整份覆蓋 —— 38 個位元組就夠,
  # 而且它與真實的漂移形狀一樣(librime 部署時把 patch 疊上去)。
  # 實測(rime_console,emulator-5554):疊上去之後送 `niG`,
  # `'n' -> 未消費`、`'i' -> 未消費`,而 `MGGAM` 的候選仍然是 你好 # ni hao ——
  # 也就是說**讀音列得出來、改寫卻做不到**,正是這個情境要驗的那個狀態。
  if [ "$SCENARIO" = "stale-schema" ]; then
    adbs shell "run-as $IME_PKG mkdir -p files/rime/user" >/dev/null 2>&1
    printf 'patch:\n  speller/alphabet: "ADGJMPTW"\n' \
      | adbs shell "run-as $IME_PKG sh -c 'cat > files/rime/user/$SCHEMA.custom.yaml'" \
      >/dev/null 2>&1
    PLANTED_SCHEMA="$(adbs shell "run-as $IME_PKG cat files/rime/user/$SCHEMA.custom.yaml" \
      2>/dev/null | tr -d '\r' | tr '\n' ' ')"
    if ! printf '%s' "$PLANTED_SCHEMA" | grep -q ADGJMPTW; then
      fail "$LAYOUT:情境的方案 patch 沒放進去(讀回來是「$PLANTED_SCHEMA」)—— 情境沒有成立"
      continue
    fi
    info "情境已植入:$SCHEMA.custom.yaml → alphabet 'ADGJMPTW'"
  fi
  # 收掉植入的方案。呼叫兩次也沒關係(rm -f);沒有植入時是 no-op。
  unplant_stale_schema() {
    [ "$SCENARIO" = "stale-schema" ] || return 0
    adbs shell "run-as $IME_PKG rm -f files/rime/user/$SCHEMA.custom.yaml" >/dev/null 2>&1
    # ⚠ 確認用 `test -f`,**不可以 grep `ls` 的輸出**:檔案不存在時 ls 印的是
    #   `ls: files/rime/user/t9_pinyin.custom.yaml: No such file or directory`
    #   —— 那句話裡就有那個檔名,於是「刪掉了」被判成「還在」。
    #   第一版就是這樣寫的,實測把一次**乾淨的**清理報成失敗。
    local left
    left="$(adbs shell \
      "run-as $IME_PKG sh -c 'test -f files/rime/user/$SCHEMA.custom.yaml && echo YES || echo NO'" \
      2>/dev/null | tr -d '\r' | tail -1)"
    if [ "$left" != "NO" ]; then
      fail "$LAYOUT:情境植入的 $SCHEMA.custom.yaml 刪不掉(test -f 回「$left」)——"
      fail "  它會變成下一支腳本的地雷:共用模擬器上,下一支會對著一個單編碼方案跑。"
      return 1
    fi
    info "情境植入的 $SCHEMA.custom.yaml 已移除"
  }

  SRC_LAYOUT="$ROOT/core/layouts/$LAYOUT.yaml"
  adbs shell "run-as $IME_PKG mkdir -p files/rime/user/layouts" >/dev/null 2>&1
  PLANTED_NOTE=""
  if [ "$PLANT" = "bad-slot-ids" ]; then
    PLANTED_NOTE="(已植入 bad-slot-ids)"
    SED_SLOTS='s/^    syllable_slots: .*/    syllable_slots: ["nope_1", "nope_2", "nope_3"]/'
  else
    SED_SLOTS='s/^__never__$/&/'
  fi
  sed -e "s/^for_schema:.*/for_schema: [\"$SCHEMA\"]/" \
      -e "s/^auto_for_schema:.*/auto_for_schema: [\"$SCHEMA\"]/" \
      -e "s/^deprecated:.*/deprecated: false/" \
      -e "$SED_SLOTS" "$SRC_LAYOUT" \
    | adbs shell "run-as $IME_PKG sh -c 'cat > files/rime/user/layouts/$LAYOUT.yaml'" >/dev/null 2>&1

  # ── 設成預設輸入法(並確認)───────────────────────────────────────
  IME_NOW=""
  for _ in $(seq 1 20); do
    adbs shell ime enable "$IME_ID" >/dev/null 2>&1
    adbs shell ime set "$IME_ID" >/dev/null 2>&1
    IME_NOW="$(adbs shell settings get secure default_input_method 2>/dev/null | tr -d '\r')"
    [ "$IME_NOW" = "$IME_ID" ] && break
    sleep 1
  done
  if [ "$IME_NOW" != "$IME_ID" ]; then
    fail "$LAYOUT:設不成預設輸入法(現在是 ${IME_NOW:-<空>})—— 接下來會對著別的鍵盤打字"
    continue
  fi

  adbs logcat -c >/dev/null 2>&1
  adbs shell am force-stop "$TARGET_PKG" >/dev/null 2>&1
  adbs shell am start -n "$TARGET_ACT" --es field text >/dev/null 2>&1
  sleep 4
  FIELD_XY="$(adbs shell "uiautomator dump /sdcard/b.xml >/dev/null 2>&1; cat /sdcard/b.xml" 2>/dev/null \
    | tr -d '\r' | python3 -c '
import sys, re, xml.etree.ElementTree as ET
try: root = ET.fromstring(sys.stdin.read())
except Exception: sys.exit(0)
for n in root.iter("node"):
    if n.get("content-desc") == "rime_matrix_input":
        m = re.match(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", n.get("bounds",""))
        if m:
            a,b,c,d = map(int, m.groups()); print((a+c)//2, (b+d)//2)
        break
')"
  # shellcheck disable=SC2086
  [ -n "$FIELD_XY" ] && adbs shell input tap $FIELD_XY >/dev/null 2>&1
  READY=0
  READY_WHY=""
  if READY_WHY="$(rime_wait_ready 120)"; then READY=1; fi
  sleep 3

  # ⚠ 在這一段以前,**三種完全不同的失敗**都長成同一句
  #   「$LAYOUT:裝置上載入的卻是 qwerty」:
  #     (a) 裝置上根本不是我們要驗的那一份 APK(共用模擬器,別條線裝了它自己的)
  #     (b) librime 部署失敗 —— 鍵盤起得來,但一個方案都沒編出來,於是停在 qwerty
  #     (c) 佈局判斷真的寫壞了
  #   這一輪 (a) 與 (b) 各發生過一次,兩次都花了一整輪才查出來不是產品的問題。
  #   所以先把前兩個問清楚,問清楚了才輪到第三個。
  check_apk_identity "$LAYOUT" || continue
  if [ "$READY" -ne 1 ]; then
    DEPLOY_ERR="$(adbs shell "run-as $IME_PKG cat files/rime/log/rime.android.ERROR" 2>/dev/null \
      | tr -d '\r' | grep '^E' | head -3 | tr '\n' ' ')"
    fail "$LAYOUT:引擎沒有準備好(問 harness 得到:${READY_WHY:-<沒有回應>})。"
    if [ "${READY_WHY:-}" = "<harness 沒有回應>" ]; then
      fail "  ⚠ **沒有人接那個廣播**。最可能的原因是裝置上這一份不是 debug 建置"
      fail "    ($RIME_STATE_RECEIVER 只存在於 src/debug/),而不是引擎起不來。"
      fail "    兩件事的處置完全不同,所以這裡分開講。"
    else
      fail "  phase 不是 READY —— **librime 部署沒成功**,不是佈局的問題。"
    fi
    fail "  librime 的 ERROR log:${DEPLOY_ERR:-<空>}"
    fail "  最常見的成因是 worktree 少了 core/data/user(裡面只有一個"
    fail "  default.custom.yaml,它把 schema_list 收斂成本專案實際打包的那幾個)。"
    fail "  缺了它 librime 會去找 cangjie5 / quick5,部署整個失敗:"
    fail "    ln -sfn /home/lc/rime/core/data/user <worktree>/core/data/user"
    continue
  fi

  ACTIVE="$(adbs logcat -d 2>/dev/null | tr -d '\r' | sed -n 's/.*佈局 . \([a-zA-Z0-9_-]*\).*/\1/p' | tail -1)"
  if [ -z "$ACTIVE" ]; then
    fail "$LAYOUT:裝置沒有回報任何佈局 —— 前景鍵盤可能根本不是我們的"
    adbs exec-out screencap -p > "$LOUT/fail-nolayout.png" 2>/dev/null
    continue
  fi
  if [ "$ACTIVE" != "$LAYOUT" ]; then
    fail "$LAYOUT:裝置上載入的卻是 $ACTIVE。驗到別份佈局卻報綠燈比沒測更糟。"
    continue
  fi
  info "裝置確認:佈局=$ACTIVE $PLANTED_NOTE"

  # ── 幾何 ────────────────────────────────────────────────────────
  WM_SIZE="$(adbs shell wm size 2>/dev/null | tr -d '\r' | sed -n 's/.*: *\([0-9]*x[0-9]*\).*/\1/p' | tail -1)"
  WM_DENS="$(adbs shell wm density 2>/dev/null | tr -d '\r' | sed -n 's/.*: *\([0-9]*\).*/\1/p' | tail -1)"
  python3 "$ROOT/scripts/layout_geom.py" --root "$ROOT" --layout "$LAYOUT" --theme "$THEME" \
    --screen "$WM_SIZE" --density "$WM_DENS" --json > "$LOUT/keymap.json" 2>"$LOUT/geom.err" || {
      fail "$LAYOUT:座標計算失敗,見 $LOUT/geom.err"; continue; }

  # 打字順序 M G G A M(= ni + hao)。鍵 id 由方案契約的 keysym 反查,
  # 不寫死 —— 三份佈局的 id 各不相同(k_mno / k6 …)。
  KEY_M="$(grep -m1 -oE '\{ *id: *"[^"]+".*keysym: *"M" *\}' "$SRC_LAYOUT" | sed -n 's/.*id: *"\([^"]*\)".*/\1/p')"
  KEY_G="$(grep -m1 -oE '\{ *id: *"[^"]+".*keysym: *"G" *\}' "$SRC_LAYOUT" | sed -n 's/.*id: *"\([^"]*\)".*/\1/p')"
  KEY_A="$(grep -m1 -oE '\{ *id: *"[^"]+".*keysym: *"A" *\}' "$SRC_LAYOUT" | sed -n 's/.*id: *"\([^"]*\)".*/\1/p')"
  # 第 4 關要打 P G M(= qin,也 = pin)。
  KEY_P="$(grep -m1 -oE '\{ *id: *"[^"]+".*keysym: *"P" *\}' "$SRC_LAYOUT" | sed -n 's/.*id: *"\([^"]*\)".*/\1/p')"
  if [ -z "$KEY_M" ] || [ -z "$KEY_G" ] || [ -z "$KEY_A" ] || [ -z "$KEY_P" ]; then
    fail "$LAYOUT:找不到送 M/G/A/P 的鍵(M=${KEY_M:-?} G=${KEY_G:-?} A=${KEY_A:-?} P=${KEY_P:-?})"
    continue
  fi

  GRID_H="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['grid_height_px'])" "$LOUT/keymap.json")"
  BAR_PX="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['bar_height_px'])" "$LOUT/keymap.json")"
  BASE_INSET=""   # 每一份佈局重量一次(列數不同,格線區高度也不同)
  read_frame || { fail "$LAYOUT:讀不到 IME 視窗 frame"; continue; }

  tap_key() {
    # 每一次點擊前都重讀 frame:打第一個字之後上方橫排就出現了,整個格線區
    # 會跟著移動,拿舊座標點下去會落在隔壁列(或兩鍵之間的縫裡)。
    read_frame || return 1
    local kid="$1" xy
    xy="$(python3 "$ROOT/scripts/layout_geom.py" --root "$ROOT" --layout "$LAYOUT" --theme "$THEME" \
          --screen "$WM_SIZE" --density "$WM_DENS" --key "$kid" --grid-top "$GRID_TOP" 2>/dev/null)"
    [ -n "$xy" ] || return 1
    # shellcheck disable=SC2086
    adbs shell input tap $xy >/dev/null 2>&1
    sleep 0.8
  }
  shot() { adbs shell screencap -p "/sdcard/syl.png" >/dev/null 2>&1; adbs pull /sdcard/syl.png "$1" >/dev/null 2>&1; }

  # ⚠ 打字**之前**先拍一張。消歧欄該出現的那幾格,是「打字前後真的變了」的
  #   那幾格 —— 用變化定位比用幾何模型定位可靠得多:
  #   · 幾何模型實測在 cn-t9-pinyin-numrow 上與渲染器差了約 75px;
  #   · 而整條左欄從格線頂端裁到底,會把數字列與 `!@#` 那顆鍵一起裁進來,
  #     OCR 於是讀出「ee ni mi lot」「ne mi o」這種夾雜垃圾的字串 ——
  #     CI 上就是這樣紅的,而畫面上明明白白寫著 ni 與 mi。
  #   而且這樣一來,斷言從「這一塊讀得到 ni」變成
  #   「**打字後才出現**的那幾格讀得到 ni」,比原本更嚴。
  # ⚠ 底圖要在畫面靜止之後才拍。鍵盤是滑上來的,拍太早的話「打字前後的差異」
  #   會變成整片都在動 —— 實測那一次:整條左欄被併成**一個**橫帶,
  #   點擊座標算成整欄的中點(y=2709,第三列的空格位),於是那一下把組字取消掉,
  #   第二關再比就變成「和底圖一模一樣」,0 個橫帶。
  #   徵狀看起來像功能壞了,其實是底圖拍早了。
  wait_stable() {
    local a="$LOUT/.s1.png" b="$LOUT/.s2.png" i
    for i in $(seq 1 12); do
      shot "$a"; sleep 0.4; shot "$b"
      if python3 - "$a" "$b" <<'PYS'
import sys
from PIL import Image, ImageChops
try:
    a = Image.open(sys.argv[1]).convert("L")
    b = Image.open(sys.argv[2]).convert("L")
except Exception:
    sys.exit(1)
if a.size != b.size:
    sys.exit(1)
bbox = ImageChops.difference(a, b).point(lambda p: 255 if p > 24 else 0).getbbox()
# 狀態列的時鐘一直在變,所以允許極小面積的差異。
if bbox is None:
    sys.exit(0)
area = (bbox[2] - bbox[0]) * (bbox[3] - bbox[1])
sys.exit(0 if area < a.width * a.height // 200 else 1)
PYS
      then
        rm -f "$a" "$b"
        return 0
      fi
      sleep 0.6
    done
    rm -f "$a" "$b"
    return 1
  }
  wait_stable || { fail "$LAYOUT:畫面一直在動,拍不到穩定的底圖"; continue; }
  shot "$LOUT/0-idle.png"

  for k in "$KEY_M" "$KEY_G" "$KEY_G" "$KEY_A" "$KEY_M"; do
    tap_key "$k" || { fail "$LAYOUT:點不到鍵 $k"; continue 2; }
  done

  # ── 消歧欄該在哪一塊 ────────────────────────────────────────────
  # 佈局的預設層宣告了 syllable_slots → 左側直欄;否則 → 候選列上方一橫排
  # (這正是退化規則(一),4 欄舊版走的就是這條)。
  SLOT_LINE="$(grep -m1 -E '^    syllable_slots:' "$SRC_LAYOUT" || true)"
  if [ "$PLANT" = "bad-slot-ids" ] && [ -n "$SLOT_LINE" ]; then
    SLOT_LINE='    syllable_slots: ["nope_1", "nope_2", "nope_3"]'
  fi

  # 裁切 + OCR。⚠ 一定要避開候選列(理由見 read_frame)。
  # 第三個參數 = 要點的那一個讀音(算 TAP 座標用),預設 ni。
  # 第 4 關要點的是 pin,寫死 ni 的話它會點到 qin 那一格 —— 而畫面看起來
  # 完全正常(有一格被點亮了),只是點錯了一格。
  ocr_region() {
    local png="$1" outtxt="$2" want="${3:-ni}"
    python3 - "$png" "$outtxt" "$LOUT/keymap.json" "$GRID_TOP" "$FRAME_TOP" "$WM_DENS" \
             "$SLOT_LINE" "$TESSERACT" "$TESSDATA" "$LOUT/0-idle.png" "$want" <<'PY'
import json, subprocess, sys, os, re
from PIL import Image, ImageOps
png, outtxt, keymap, grid_top, frame_top, dens, slot_line, tess, tessdata, idle, want = \
    sys.argv[1:12]
grid_top, frame_top, dens = int(grid_top), int(frame_top), int(dens)
im = Image.open(png).convert("L")
base = Image.open(idle).convert("L") if os.path.isfile(idle) else None
if base is not None and base.size != im.size:
    base = None          # 尺寸變了(旋轉?)就別比,寧可退回舊做法也不要比錯
env = dict(os.environ)
if tessdata:
    env["TESSDATA_PREFIX"] = tessdata

def ocr(img, tag, psms):
    """一格(或一排)上印的字母。認不出來回空字串。

    ⚠ **先裁到墨跡再放大。** 一顆鍵是 227x170 px,而上面只有 `ni` 兩個字母置中;
    直接整格丟給 tesseract,它看到的是一大片空白加中間一點點字,psm 7 幾乎讀不出
    東西(實測讀成 `a` / `pe`)。裁到墨跡的外框、再統一放大到固定字高,
    辨識率才穩定。
    """
    work = ImageOps.autocontrast(img, cutoff=1)
    # ⚠ 門檻要**相對**,不能寫死。左欄是淺灰底深灰字,上方橫排是白底淺灰字 ——
    #   寫死 110 的那一版把上方橫排整個判成空白(實測:t9-pinyin 讀出空字串,
    #   而同一張圖在沒有裁墨跡時是讀得到 ni mi 的)。
    lo, hi = work.getextrema()
    if hi - lo < 12:
        return ""            # 整塊是同一個顏色 = 沒有字
    thr = lo + (hi - lo) * 0.45
    mask = work.point(lambda p: 255 if p <= thr else 0)
    bb = mask.getbbox()
    if bb is None:
        return ""            # 空格位(spacer),本來就沒有字
    pad = 10
    work = work.crop((max(0, bb[0] - pad), max(0, bb[1] - pad),
                      min(work.width, bb[2] + pad), min(work.height, bb[3] + pad)))
    if work.height < 4 or work.width < 4:
        return ""
    # 字高拉到 ~160px 再送。80px 那一版把 cn-t9-pinyin 的 `ni` 讀成 `ne` ——
    # 那一格是淺灰底上的深灰字,`i` 的點在低解析度下會併進字身。
    scale = max(1, int(round(160.0 / work.height)))
    work = work.resize((work.width * scale, work.height * scale), Image.LANCZOS)
    work = ImageOps.expand(work, border=30, fill=255)
    work.save(outtxt + "." + tag + ".png")
    # psm 8 = 單一詞(左欄一格就是一個詞),psm 7 = 單行(上方橫排一行三個詞)。
    # ⚠ 順序由呼叫端決定:對上方橫排先用 psm 8 的話,它只會吐出一個詞,
    #   `mi` 就不見了 —— 而斷言要的是「ni 與 mi 都在」。
    #
    # ⚠ **不要跨 psm 取聯集。** 在真的 band 圖上掃過一輪(12 張、4 個 psm、
    #   oem 與 whitelist 各兩種)結果很乾脆:
    #     · 一格一個詞的直欄 → psm 8 / 13 全對,psm 6 / 7 一律吐 `al` `aal`
    #     · 一行多個詞的橫排 → psm 6 / 7 全對,psm 8 / 13 黏成 `nimi`
    #   whitelist 與 oem 完全不影響。所以正確的 psm 只有一個,聯集只會把
    #   另一個 psm 的垃圾一起收進來(實測:`ni al mi aal`)。
    #   呼叫端給的第一個就是對的那一個,後面的只在前面讀不出東西時才試。
    for psm in psms:
        r = subprocess.run(
            [tess, outtxt + "." + tag + ".png", "stdout", "--psm", psm,
             "-c", "tessedit_char_whitelist=abcdefghijklmnopqrstuvwxyz",
             # 關掉字典:它會把 `ni` 修成英文字。我們要的是鍵面上印的那幾個字母。
             "-c", "load_system_dawg=0", "-c", "load_freq_dawg=0"],
            capture_output=True, text=True, env=env)
        t = re.sub(r"[^a-z]+", " ", r.stdout.lower()).strip()
        if t:
            return t
    return ""

ids = re.findall(r'"([^"]+)"', slot_line) if slot_line.strip() else []
tokens, boxes = [], []
rects = []
if ids:
    keys = {k["id"]: k for k in json.load(open(keymap))["keys"] if k.get("id")}
    rects = [keys[i] for i in ids if i in keys]

tap_xy = ""
if ids and not rects:
    # ⚠ 佈局宣告了格位,但那些 id 在這一層裡不存在(有人把 pu_comma 改名了)。
    #   **絕不可以退回去裁「候選列上方那一條」** —— 那時候上方橫排根本沒有渲染,
    #   裁到的會是**候選列本身**,而候選列的 comment 印著「ni hao」,
    #   OCR 讀得到 ni,關卡就永遠是綠的。這正是這支腳本要擋的那種假綠燈。
    open(outtxt, "w").write("")
    open(outtxt + ".tap", "w").write("")
    print("BOX=[] TEXT= TAP=- ERROR=宣告的格位 id 在這一層找不到:%s" % ",".join(ids))
    sys.exit(0)

if rects:
    # 左側直欄。
    #
    # ⚠ **不照 layout_geom 的每一格去裁。** 實測 `cn-t9-pinyin-numrow` 的模型把
    #   `pu_comma` 放在比實際渲染低 ~75px 的位置(同一份模型對 `k_mno` 卻是準的),
    #   於是三個框全部裁在格與格之間,OCR 讀出「re i ry」而畫面上明明寫著 ni / mi。
    #   幾何模型與渲染器對不上時,**要相信畫面**。
    #
    # 作法:取整條左欄(x 用模型的欄寬,y 從格線區頂端到視窗底端 —— 這樣一定
    # 涵蓋所有列,而且**絕不會碰到候選列**,候選列在格線區之上,它的 comment
    # 印著「ni hao」,裁進來就是永遠綠的假關卡)。再依「有墨跡的橫帶」把欄切成
    # 一格一格,每一帶就是一個讀音。
    x0 = min(r["x"] for r in rects); x1 = max(r["x"] + r["w"] for r in rects)
    # ⚠ **格線頂端要往上讓一點,否則 `i` 的那一點會被切掉。**
    #   CI 上實際發生過:`grid_top = FRAME_BOT - GRID_H` 算出來的線正好壓在
    #   `ni` 的字身上緣,那一點落在線的上面 —— 裁出來的圖是 `nl`,
    #   tesseract 讀成 `ne`,而畫面完全正確。(本機模擬器解析度不同,
    #   那一點剛好在線下面,所以本機一直是綠的 —— 又一個「換台機器才發作」。)
    #
    #   讓多少要有上限:候選列就在格線區正上方,而它的 comment 印著「ni hao」——
    #   讓過頭就會裁到候選列,那是這支腳本從第一天就在防的假綠燈。
    #   兩個上限取小的:格線區高度的 6%,以及「視窗頂端到格線頂端」的 25%
    #   (候選列的文字在它自己那一條的中間,25% 連它的下緣留白都碰不到)。
    lift = min(int(0.06 * max(0, im.height - grid_top)),
               int(0.25 * max(0, grid_top - frame_top)))
    col = (max(0, x0), max(0, grid_top - lift), min(im.width, x1), im.height)
    boxes.append(col)
    strip = im.crop(col)
    if base is not None:
        # 「有沒有變」比「有沒有墨跡」精準:數字列與 `!@#` 那顆鍵一直都在,
        # 但它們不會因為打了字而改變,於是自然被排除。
        b = base.crop(col)
        sp, bp = strip.load(), b.load()
        rowink = [
            sum(1 for x in range(strip.width) if abs(sp[x, y] - bp[x, y]) > 24)
            for y in range(strip.height)
        ]
        # 少量雜訊(抗鋸齒、游標)不算變化。
        floor = max(2, strip.width // 20)
        rowink = [n if n >= floor else 0 for n in rowink]
        w = strip
    else:
        w = ImageOps.autocontrast(strip, cutoff=1)
        lo, hi = w.getextrema()
        thr = lo + (hi - lo) * 0.45 if hi > lo else 0
        px = w.load()
        rowink = [sum(1 for x in range(w.width) if px[x, y] <= thr) for y in range(w.height)]
    # 先切出所有的墨跡段(不論多短),之後再合併、再篩。
    runs, cur = [], None
    for y, n in enumerate(rowink):
        if n > 0 and cur is None:
            cur = y
        elif n == 0 and cur is not None:
            runs.append((cur, y))
            cur = None
    if cur is not None:
        runs.append((cur, w.height))

    # ⚠ **`i` 的那一點是獨立的一段。** 直接用「長度 >= 8」篩,那一點會被丟掉,
    #   而裁出來的圖就變成 `nl` —— CI 上實際發生過:tesseract 讀成 `ne`,
    #   看起來像功能壞了,其實是這裡把字裁壞了。(本機的模擬器解析度不同,
    #   那一點剛好併進來了,所以本機一直是綠的。)
    #   所以:間距小於「較高那一段的 60%」就當成同一個字併起來。
    merged = []
    for r in runs:
        if merged:
            gap = r[0] - merged[-1][1]
            tall = max(r[1] - r[0], merged[-1][1] - merged[-1][0])
            if gap <= max(6, int(tall * 0.6)):
                merged[-1] = (merged[-1][0], r[1])
                continue
        merged.append(r)
    bands = [r for r in merged if r[1] - r[0] >= 8]
    # 一個橫帶佔掉半條欄以上,那不是一個格位 —— 幾乎一定是底圖拍在畫面還在動的
    # 時候。這種情況要**指名**,不能讓它變成「消歧欄上讀不到 ni/mi」——
    # 那句話會把人送去查一個沒壞的功能。
    if bands and max(y1 - y0 for y0, y1 in bands) > w.height * 0.5:
        open(outtxt, "w").write("")
        open(outtxt + ".tap", "w").write("")
        print("BOX=%s TEXT= TAP=- ERROR=左欄整片都在變,底圖不可信(鍵盤可能還在動)"
              % (boxes,))
        sys.exit(0)
    for n, (by0, by1) in enumerate(bands):
        pad = 8
        b = (0, max(0, by0 - pad), w.width, min(w.height, by1 + pad))
        tok = ocr(strip.crop(b), "band%d" % n, ("8", "13"))
        tokens.append(tok)
        # 點的是**我們剛剛在畫面上讀到的那一格**,不是模型算出來的座標。
        if tok and want in tok.split() and not tap_xy:
            tap_xy = "%d %d" % ((col[0] + col[2]) // 2, col[1] + (by0 + by1) // 2)
    if not tap_xy and bands:
        by0, by1 = bands[0]
        tap_xy = "%d %d" % ((col[0] + col[2]) // 2, col[1] + (by0 + by1) // 2)
else:
    # 上方橫排:貼著 IME 視窗頂端,高度 = 主題的 height(預設 40dp)。
    h = int(round(40 * dens / 160.0))
    b = (0, frame_top, im.width, min(im.height, frame_top + h))
    boxes.append(b)
    if base is not None:
        d = im.crop(b)
        e = base.crop(b)
        dp, ep = d.load(), e.load()
        changed = sum(
            1
            for y in range(d.height)
            for x in range(0, d.width, 4)
            if abs(dp[x, y] - ep[x, y]) > 24
        )
        # 門檻是量出來的,不是猜的:實測 t9-pinyin 這一條在打字前後有 172 個
        # 取樣點變了("ni mi" 兩個小字),而完全沒變的區域是 0。所以這裡要的是
        # 一個「有沒有」的下限,不是「變了多少」的比例 —— 比例那一版訂在 226,
        # 把一條**畫對了**的消歧欄判成沒出現。
        if changed < 24:
            # 這一條在打字前後一模一樣 —— 它不是消歧欄,是別的東西。
            open(outtxt, "w").write("")
            open(outtxt + ".tap", "w").write("")
            print("BOX=%s TEXT= TAP=- ERROR=候選列上方那一條在打字前後沒有變化" % (boxes,))
            sys.exit(0)
    tokens.append(ocr(im.crop(b), "row", ("7", "6")))
    # 上方橫排也要算得出「want 那一格在哪裡」。第 4 關要點的是 pin,而讀音的
    # 順序就是引擎的順序(PGM 的第一個是 qin)—— 照序號猜會點到 qin,
    # 而畫面上確實有一格被點亮了,看起來完全正常,只是點錯了一格。
    # 作法與左欄那一段對稱:依「有墨跡的直行」把整排切成一格一格。
    row = ImageOps.autocontrast(im.crop(b), cutoff=1)
    rlo, rhi = row.getextrema()
    if rhi - rlo >= 12:
        rthr = rlo + (rhi - rlo) * 0.45
        rpx = row.load()
        colink = [sum(1 for y in range(row.height) if rpx[x, y] <= rthr)
                  for x in range(row.width)]
        cruns, ccur = [], None
        for x, n in enumerate(colink):
            if n > 0 and ccur is None:
                ccur = x
            elif n == 0 and ccur is not None:
                cruns.append((ccur, x))
                ccur = None
        if ccur is not None:
            cruns.append((ccur, row.width))
        # 字母之間的縫比 chip 之間的縫小得多(chip 左右各有 12dp padding),
        # 所以用「小於排高的三分之一就併起來」把一個詞收成一段。
        cmerged = []
        for r in cruns:
            if cmerged and r[0] - cmerged[-1][1] <= max(6, row.height // 3):
                cmerged[-1] = (cmerged[-1][0], r[1])
                continue
            cmerged.append(r)
        for n, (bx0, bx1) in enumerate(cmerged):
            if bx1 - bx0 < 6:
                continue
            chip = row.crop((max(0, bx0 - 6), 0, min(row.width, bx1 + 6), row.height))
            tok = ocr(chip, "chip%d" % n, ("8", "13"))
            if tok and want in tok.split() and not tap_xy:
                tap_xy = "%d %d" % ((bx0 + bx1) // 2, (b[1] + b[3]) // 2)

text = " ".join(t for t in tokens if t).strip()
open(outtxt, "w").write(text)
open(outtxt + ".tap", "w").write(tap_xy)
print("BOX=%s TEXT=%s TAP=%s" % (boxes, text, tap_xy or "-"))
PY
  }

  # ── 候選列那一條 ────────────────────────────────────────────────
  #
  # ⚠ **只有第 4 關與 stale-schema 情境可以用它。** 這支腳本從第一天就在躲開
  #   候選列:它的 comment 本來就印著 `ni hao`,裁進消歧欄的框裡,OCR 就永遠
  #   讀得到 ni,關卡永遠綠(見 read_frame 與 ocr_region 的註解)。
  #   這裡**故意**去讀它,因為第 4 關問的正是「候選列上還有沒有 qin」。
  #   為了不讓它變成另一個假綠燈,每一次斷言「沒有」之前,都先在同一塊區域
  #   斷言過一次「有」。
  #
  # 裁的是 IME 視窗頂端到格線區頂端的一整條。above_candidates 風格時
  # 那一條裡還有上方的讀音橫排 —— 刻意不扣掉:收斂之後那一排本來就該消失,
  # 兩者都不准再有 qin,分開裁只會多一組會漂移的座標。
  ocr_candbar() {
    local png="$1" outtxt="$2"
    python3 - "$png" "$outtxt" "$GRID_TOP" "$FRAME_TOP" "$TESSERACT" "$TESSDATA" <<'PY'
import os, re, statistics, subprocess, sys
from PIL import Image, ImageOps
png, outtxt, grid_top, frame_top, tess, tessdata = sys.argv[1:7]
grid_top, frame_top = int(grid_top), int(frame_top)

# ⚠ **不要裁到 grid_top。**(2026-08-13:grid_top 的公式已經修好了,不再是
#   `FRAME_BOT - grid_height_px`;但這一段補償仍然要留著 —— 它擋的是
#   「候選列與第一排按鍵之間那條縫」,那與公式對不對是兩件事。)實測
#   舊公式算出來的線落在候選列底下約 50 px 的地方 —— 裁到那裡會把**第一排按鍵的頂端**
#   一起裁進來,而 tesseract 是整塊一起讀的,那半排被切掉的大字會把整張圖
#   讀成亂碼。
#
#   這不是假設,是 ship 這一輪 CI 上抓到的:同一次跑,三份九宮格佈局的
#   候選列**逐像素完全相同**(cn-t9-pinyin 與 cn-t9-pinyin-numrow 的
#   1600..1719 兩張圖差 0 個像素),而裁進來的那 62 px 差 30153 個像素 ——
#   cn-t9-pinyin 那一份底下是半排「分詞 / abc / def」,讀出來是「ys y ae」;
#   numrow 那一份底下是半排數字(0-9 不在 a-z 白名單裡,等於沒有東西),
#   讀出來是「gppin qin gin y fyzaaqfi」,含 qin 就過了。
#   **同樣的候選列,一個過一個不過,差別只在裁進來的那半排按鍵。**
#
#   所以改成從 grid_top **往上找**候選列與格線區之間那條安靜的縫。
#   為什麼不從 frame_top 往下找:上方橫排那幾份佈局(t9-pinyin)的
#   frame_top 是**橫排**的頂端,往下找會先撞到橫排與候選列之間那條縫,
#   裁在那裡等於把候選列整條丟掉。
_MIN_BAR_PX, _QUIET_SD, _QUIET_RUN = 40, 8, 4

def _bar_bottom(im, frame_top, grid_top):
    px = im.load()
    xs = range(0, im.width, 7)
    run = 0
    for y in range(min(grid_top, im.height) - 1, frame_top + _MIN_BAR_PX - 1, -1):
        if statistics.pstdev([px[x, y] for x in xs]) < _QUIET_SD:
            run += 1
            if run >= _QUIET_RUN:
                return y + run - 1          # 那條縫最下面那一列
        else:
            run = 0
    return grid_top                          # 找不到縫就退回舊行為,不會裁成空的

im = Image.open(png).convert("L")
_top = max(0, frame_top)
_bot = _bar_bottom(im, _top, max(0, min(im.height, grid_top)))
box = (0, _top, im.width, _bot)
_trim = max(0, min(im.height, grid_top)) - _bot
if box[3] - box[1] < 8:
    open(outtxt, "w").write("")
    print("BOX=%s TEXT= ERROR=候選列那一條的高度不合理(%d px)" % (box, box[3] - box[1]))
    sys.exit(0)
work = ImageOps.autocontrast(im.crop(box), cutoff=1)
# comment 是候選列上最小的字(§8.6.6 的 comment.size)。放大兩倍再送,
# 理由與 ocr_region 那一段一樣:小字直接丟給 tesseract 讀不出來。
work = work.resize((work.width * 2, work.height * 2), Image.LANCZOS)
work.save(outtxt + ".png")
env = dict(os.environ)
if tessdata:
    env["TESSDATA_PREFIX"] = tessdata
text = ""
# psm 6 = 一整塊文字(候選列是一行,但字之間隔得很開);讀不出來再試
# psm 11(稀疏文字)。順序同 ocr_region:第一個就是對的那一個。
for psm in ("6", "11"):
    r = subprocess.run(
        [tess, outtxt + ".png", "stdout", "--psm", psm,
         "-c", "tessedit_char_whitelist=abcdefghijklmnopqrstuvwxyz",
         "-c", "load_system_dawg=0", "-c", "load_freq_dawg=0"],
        capture_output=True, text=True, env=env)
    t = re.sub(r"[^a-z]+", " ", r.stdout.lower()).strip()
    if t:
        text = t
        break
open(outtxt, "w").write(text)
print("BOX=%s TRIM=%dpx%s TEXT=%s"
      % (box, _trim, "" if _trim else "(沒找到候選列與格線區之間那條縫,退回裁到 grid_top)", text))
PY
  }

  # ── 第 2 關:第一個音節 ─────────────────────────────────────────
  read_frame || { fail "$LAYOUT:讀不到 IME 視窗 frame"; continue; }
  shot "$LOUT/1-typed.png"
  OCR1="$(ocr_region "$LOUT/1-typed.png" "$LOUT/1-typed.txt" 2>&1)"; RC1=$?
  info "$OCR1"
  # 裁切/OCR 自己壞掉 ≠ 畫面上沒有那幾個字。兩者報成同一句話的話,
  # 下一個人會去查一個根本沒壞的功能(這正是這一輪發生的事)。
  if [ "$RC1" -ne 0 ]; then
    fail "$LAYOUT:裁切/OCR 這一步自己失敗了(exit $RC1),不是畫面的問題:$OCR1"
    continue
  fi
  T1="$(cat "$LOUT/1-typed.txt" 2>/dev/null || echo)"

  if [ "$SCENARIO" = "stale-schema" ]; then
    # 正向對照先跑:候選列上必須讀得到 ni。舊方案上 comment 還在
    # (實測 MGGAM → 你好 # ni hao),所以讀不到只有兩種可能:app 沒在組字,
    # 或這一條裁歪了 —— 兩種都不可以被算成「消歧欄正確地沒有出現」。
    OCRC="$(ocr_candbar "$LOUT/1-typed.png" "$LOUT/1-candbar.txt" 2>&1)"; RCC=$?
    info "$OCRC"
    TC="$(cat "$LOUT/1-candbar.txt" 2>/dev/null || echo)"
    # ⚠ 這裡要的是「**開頭**是 ni 的詞」,不是「獨立的 ni」。
    #   這個情境打的是 MGGAM,comment 是**兩個音節**的 `ni hao` / `ni gan`,
    #   而中間那個空白在候選列上很窄 —— tesseract 實測讀出來的是
    #   `nihaofiw nihao nigan is`(一個詞)。用 `grep -qw ni` 會判成
    #   「讀不到 ni」,於是一個**正確**的情境被報成「對照組沒成立」。
    #   (第 4 關那一條的 comment 是單音節的 qin / pin,不會有這個問題。)
    if [ "$RCC" -ne 0 ] || ! echo "$TC" | grep -qE '(^| )ni'; then
      fail "$LAYOUT:情境的正向對照沒過 —— 候選列上讀不到 ni(「$TC」)。"
      fail "  app 可能根本沒在組字,或這一條裁歪了。在那之前不能斷言消歧欄「正確地沒出現」。"
      continue
    fi
    pass "$LAYOUT:正向對照 —— 候選列上讀得到 ni(\"$TC\"),app 確實在組字"
    # ⚠ **植入的方案一定要收掉,而且要在斷言之前收。** emulator 是四條線共用的,
    #   而 `pm clear` 只有這一支自己會下 —— 留在那裡的 `t9_pinyin.custom.yaml`
    #   會讓下一支守門腳本(verify_candbar.sh …)對著一個**單編碼方案**跑,
    #   而它完全不知道。那不是它的 bug,是我們留下的地雷。
    #   放在斷言之前,是因為斷言可能 `continue` 掉。
    unplant_stale_schema
    if echo "$T1" | grep -qwE 'ni|mi'; then
      fail "$LAYOUT:方案改寫不了精確拼音,消歧欄卻還是畫出來了(OCR:「$T1」)。"
      fail "  那是一排按下去只會讓引擎收到垃圾的鍵 —— 真機回報過「我選擇 ni 他就直接給我輸入了」。"
    else
      pass "$LAYOUT:單編碼方案上消歧欄整條沒有出現(OCR:\"$T1\")"
    fi
    continue
  fi

  if echo "$T1" | grep -qw ni && echo "$T1" | grep -qw mi; then
    pass "$LAYOUT:畫面上讀得到第一個音節 ni 與 mi(\"$T1\")"
  else
    fail "$LAYOUT:消歧欄上讀不到 ni/mi。OCR 讀到的是「$T1」。截圖 $LOUT/1-typed.png"
    continue
  fi

  # ── 點下 ni ─────────────────────────────────────────────────────
  if [ -n "$SLOT_LINE" ] && [ "$PLANT" != "bad-slot-ids" ]; then
    # 點「畫面上讀到 ni 的那一格」,座標由上一步的 OCR 一起算出來。
    # 用模型座標點的那一版,在 numrow 上會落在兩格之間 —— 而畫面是對的。
    TAP_XY="$(cat "$LOUT/1-typed.txt.tap" 2>/dev/null || echo)"
    if [ -z "$TAP_XY" ]; then fail "$LAYOUT:算不出消歧格的座標"; continue; fi
    # shellcheck disable=SC2086
    adbs shell input tap $TAP_XY >/dev/null 2>&1
    sleep 1
  else
    # 上方橫排的第一個 chip:左邊 12dp padding,再往右一點點。
    read_frame || { fail "$LAYOUT:讀不到 IME 視窗 frame"; continue; }
    PADPX=$(python3 -c "print(int(round(12*$WM_DENS/160.0)))")
    ROWH=$(python3 -c "print(int(round(40*$WM_DENS/160.0)))")
    adbs shell input tap $((PADPX + 20)) $((FRAME_TOP + ROWH / 2)) >/dev/null 2>&1
    sleep 1
  fi
  sleep 1

  # ── 第 3 關:第二個音節 ─────────────────────────────────────────
  read_frame || { fail "$LAYOUT:讀不到 IME 視窗 frame"; continue; }
  shot "$LOUT/2-picked.png"
  OCR2="$(ocr_region "$LOUT/2-picked.png" "$LOUT/2-picked.txt" 2>&1)"; RC2=$?
  info "$OCR2"
  if [ "$RC2" -ne 0 ]; then
    fail "$LAYOUT:裁切/OCR 這一步自己失敗了(exit $RC2),不是畫面的問題:$OCR2"
    continue
  fi
  T2="$(cat "$LOUT/2-picked.txt" 2>/dev/null || echo)"
  if echo "$T2" | grep -qwE 'hao|gan|gao'; then
    pass "$LAYOUT:選了 ni 之後,畫面上換成第二個音節(\"$T2\")"
  else
    fail "$LAYOUT:選了 ni 之後讀不到第二個音節(hao/gan/gao)。OCR:「$T2」。截圖 $LOUT/2-picked.png"
  fi

  # ── 第 4 關:單音節也要收斂(PGM → 點 pin)────────────────────────
  # 為什麼要獨立一關,見檔頭。第 2/3 關走的是多音節那一條,
  # 而**壞掉的是單音節那一條** —— 兩條走的是同一段程式碼,結果相反。
  step "4. $LAYOUT:單音節收斂(PGM → pin)"
  check_apk_identity "$LAYOUT(第 4 關)" || continue

  # 先把上一關留下的組字刪乾淨。刪除鍵走 librime(processHardwareKey),
  # 欄位本來就是空的,多刪幾下沒有副作用。
  for _ in $(seq 1 12); do adbs shell input keyevent 67 >/dev/null 2>&1; done
  sleep 1

  for k in "$KEY_P" "$KEY_G" "$KEY_M"; do
    tap_key "$k" || { fail "$LAYOUT:第 4 關點不到鍵 $k"; continue 2; }
  done

  read_frame || { fail "$LAYOUT:第 4 關讀不到 IME 視窗 frame"; continue; }
  shot "$LOUT/3-pgm.png"

  # ── 2026-08-13:正向對照從**候選列**改成**消歧欄** ────────────────────
  #
  # 這一關原本的對照組是「候選列上讀得到 qin」,靠的是候選旁的註解
  # (`comment`)印著讀音。而註解與消歧欄**取自同一個 comment 欄位**,
  # 上一版依 §8.6.3.1 把它關掉了(同一份讀音畫兩次,而只有註解要付寬度)——
  # 於是這個對照組在 keyboard_slot 那兩份佈局上永遠不成立,而它一不成立
  # 這一關就 `continue`,連帶把第 5 關也整個跳過。
  #
  # 對照組要問的東西沒有變:「畫面上此刻有沒有 qin」。它只是搬家了 ——
  # 從候選列搬到消歧欄,而消歧欄正是這一關真正在測的那個東西。
  # 上方橫排那一份(t9-pinyin)仍然走候選列那條路:它的讀音橫排就畫在
  # 候選列上方的同一條帶子裡。
  if [ -n "$SLOT_LINE" ]; then
    OCR3="$(ocr_region "$LOUT/3-pgm.png" "$LOUT/3-candbar.txt" qin 2>&1)"; RC3=$?
    WHERE3="消歧欄"
  else
    OCR3="$(ocr_candbar "$LOUT/3-pgm.png" "$LOUT/3-candbar.txt" 2>&1)"; RC3=$?
    WHERE3="候選列"
  fi
  info "$OCR3"
  if [ "$RC3" -ne 0 ]; then
    fail "$LAYOUT:第 4 關的裁切/OCR 自己失敗了(exit $RC3),不是畫面的問題:$OCR3"
    continue
  fi
  T3="$(cat "$LOUT/3-candbar.txt" 2>/dev/null || echo)"
  if echo "$T3" | grep -qw qin; then
    pass "$LAYOUT:打 PGM 之後,$WHERE3 上讀得到 qin(\"$T3\")"
  else
    fail "$LAYOUT:打 PGM 之後 $WHERE3 上讀不到 qin(「$T3」)—— 對照組沒成立,"
    fail "  在這之前不能斷言「點了 pin 就沒有 qin 了」。截圖 $LOUT/3-pgm.png"
    continue
  fi

  # 正向對照 ②:消歧欄上要有 pin 那一格,而且要點得到它(座標由 OCR 一起算)。
  OCR3S="$(ocr_region "$LOUT/3-pgm.png" "$LOUT/3-slots.txt" pin 2>&1)"; RC3S=$?
  info "$OCR3S"
  T3S="$(cat "$LOUT/3-slots.txt" 2>/dev/null || echo)"
  if [ "$RC3S" -ne 0 ] || ! echo "$T3S" | grep -qw pin; then
    fail "$LAYOUT:消歧欄上讀不到 pin(「$T3S」)。截圖 $LOUT/3-pgm.png"
    continue
  fi
  TAP4="$(cat "$LOUT/3-slots.txt.tap" 2>/dev/null || echo)"
  if [ -z "$TAP4" ]; then fail "$LAYOUT:算不出 pin 那一格的座標"; continue; fi
  # shellcheck disable=SC2086
  adbs shell input tap $TAP4 >/dev/null 2>&1
  sleep 2

  read_frame || { fail "$LAYOUT:第 4 關(點完)讀不到 IME 視窗 frame"; continue; }
  shot "$LOUT/4-picked-pin.png"

  # ⛔ **「還有沒有 qin」要排在「畫面動了沒有」前面。**
  #
  # 兩道防線都留著,只是順序反了會把這一關的牙齒拔掉:
  # `--plant tap-passthrough` 植入之下,點讀音那一下**本來就不會有任何效果**
  # —— 於是「畫面幾乎沒變」先踩紅並 continue,而這一關真正在守的那一條
  # (「點了 pin,上面還有 qin」)永遠跑不到。實測 2026-08-14:MOVED=0‰ 先紅,
  # 反向測試因此判成「紅的不是該紅的那一條」—— CI 的 emulator job 相對 main 由綠轉紅。
  #
  # 排好之後兩道各守各的:
  #   還讀得到 qin → 那就是缺陷本身,不必先問畫面動了沒有(畫面沒動正是症狀)。
  #   讀不到 qin   → 這時候才需要「畫面真的變了」當佐證:「讀不到 qin」有兩種成因,
  #                   一種是收斂了(要的),一種是那一塊根本沒東西可讀(空字串永遠不含
  #                   qin)—— 而後者正是這一關最容易做出來的假綠燈。
  if [ -n "$SLOT_LINE" ]; then
    OCR4="$(ocr_region "$LOUT/4-picked-pin.png" "$LOUT/4-candbar.txt" qin 2>&1)"; RC4=$?
    WHERE4="消歧欄"
  else
    OCR4="$(ocr_candbar "$LOUT/4-picked-pin.png" "$LOUT/4-candbar.txt" 2>&1)"; RC4=$?
    WHERE4="候選列"
  fi
  info "$OCR4"
  # ⚠ 收斂之後消歧欄可能整條收起來(只剩一個讀音,門檻是 2)。那時候 ocr_region
  #   會回非 0 或空字串 —— 兩者都表示「上面沒有 qin」,而那正是下面 MOVED 那一道要佐證的。
  T4="$(cat "$LOUT/4-candbar.txt" 2>/dev/null || echo)"
  MOVED="$(python3 - "$LOUT/3-pgm.png" "$LOUT/4-picked-pin.png" "$FRAME_TOP" "$FRAME_BOT" <<'PYM'
import sys
from PIL import Image, ImageChops
a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
y0, y1 = int(sys.argv[3]), int(sys.argv[4])
box = (0, max(0, y0), a.width, min(a.height, y1))
d = ImageChops.difference(a.crop(box), b.crop(box)).convert("L")
n = sum(1 for p in d.getdata() if p > 24)
print(int(round(n * 1000.0 / (d.size[0] * d.size[1]))))
PYM
)"
  info "$LAYOUT:點了 pin 之後 IME 視窗變了 ${MOVED}‰"
  if echo "$T4" | grep -qw qin; then
    fail "$LAYOUT:點了 pin,${WHERE4}上還有 qin(「$T4」)—— 候選沒有收斂。"
    fail "  這就是真機回報的那一條:改寫被判成失敗、輸入串被還原,畫面只變了 ${MOVED}‰。"
    fail "  截圖 $LOUT/4-picked-pin.png"
  elif [ "${MOVED:-0}" -lt 3 ]; then
    fail "$LAYOUT:$WHERE4 上讀不到 qin,但點了 pin 之後畫面幾乎沒變(${MOVED}‰)——"
    fail "  那一下沒有落在 pin 上,或者改寫被判成失敗、輸入串被還原。"
    fail "  這時候「讀不到 qin」不算數。截圖 $LOUT/4-picked-pin.png"
    continue
  else
    pass "$LAYOUT:點了 pin 之後 $WHERE4 上不再有 qin(\"$T4\",畫面變了 ${MOVED}‰)"
  fi
  # ── 第 5 關:沒被接管的那一格 ────────────────────────────────────
  #
  # 為什麼要獨立一關,見檔頭「第 5 關在守什麼」。第 2/3/4 關問的都是**被接管**
  # 的那幾格;而同一個判斷同時決定了**沒被接管的那一格**會怎樣,那一格在畫面上
  # 完全正常 —— 只是按下去什麼都不會發生。
  #
  # ⚠ 上方橫排風格的佈局沒有這種格位(用不到的讀音根本不畫),所以跳過。
  #   **跳過與綠燈長得一模一樣**,收尾時有 GATE5_RAN 的下界擋著。
  if [ -z "$SLOT_LINE" ]; then
    info "$LAYOUT:上方橫排風格,沒有「沒被接管的那一格」—— 第 5 關不適用"
    continue
  fi
  step "5. $LAYOUT:沒被接管的那一格(標點鍵)按不按得動"
  check_apk_identity "$LAYOUT(第 5 關)" || continue

  # 第三個格位是誰、它鍵面上畫的是什麼 —— 兩個都從佈局讀,不寫死:
  # 寫死的那一份會跟著佈局腐爛,而腐爛的樣子是「一直綠」。
  SLOT3_ID="$(printf '%s' "$SLOT_LINE" | grep -oE '"[A-Za-z0-9_]+"' | tr -d '"' | sed -n 3p)"
  SLOT3_LABEL="$(grep -A4 "id: \"$SLOT3_ID\"" "$SRC_LAYOUT" \
                 | sed -n 's/^ *label: *"\(.*\)"$/\1/p' | head -1)"
  if [ -z "$SLOT3_ID" ] || [ -z "$SLOT3_LABEL" ]; then
    fail "$LAYOUT:第 5 關讀不出第三個格位(id=${SLOT3_ID:-<空>} label=${SLOT3_LABEL:-<空>})。"
    fail "  syllable_slots 或那顆鍵的 label 改過了 —— 這一關沒有東西可以驗,不能靜靜跳過。"
    continue
  fi

  # 宿主輸入框現在的文字。**問輸入框,不要問 OCR**:「有沒有上屏」這件事只有
  # 輸入框知道,而候選列上印著什麼與上屏了什麼是兩回事(第 4 關吃過這個虧)。
  field_text() {
    adbs shell "uiautomator dump /sdcard/s5.xml >/dev/null 2>&1; cat /sdcard/s5.xml" 2>/dev/null \
      | tr -d '\r' | python3 -c '
import sys, xml.etree.ElementTree as ET
try: root = ET.fromstring(sys.stdin.read())
except Exception: sys.exit(0)
for n in root.iter("node"):
    if n.get("content-desc") == "rime_matrix_input":
        sys.stdout.write(n.get("text", ""))
        break
'
  }

  # 「沒被接管的那一格」在畫面上的位置,以及它在打字前後變了幾個像素。
  #
  # ⚠ 位置是**量出來的**,不是問幾何模型:模型在 cn-t9-pinyin 上把 pu_question
  #   放在比實際渲染低約 50px 的地方(numrow 上約 75px,見 ocr_region)。
  #   作法是取前兩格**變了**的那兩條橫帶的中心,外推一格 —— 三個格位在兩份
  #   佈局裡都坐在連續三列等高的列上,所以帶距就是列距。
  #   外推前要求剛好兩條橫帶;不是兩條就指名說出來(讀音數變了 / 底圖拍壞了),
  #   不可以默默拿一個猜的座標往下走。
  untouched_cell() {
    python3 - "$1" "$LOUT/0-idle.png" "$LOUT/keymap.json" "$GRID_TOP" "$SLOT_LINE" <<'PY'
import json, re, sys
from PIL import Image, ImageChops
shot, idle, keymap, grid_top, slot_line = sys.argv[1:6]
grid_top = int(grid_top)
im = Image.open(shot).convert("L")
base = Image.open(idle).convert("L")
if base.size != im.size:
    print("ERROR=底圖與截圖尺寸不同(旋轉了?)"); sys.exit(0)
ids = re.findall(r'"([^"]+)"', slot_line)
keys = {k["id"]: k for k in json.load(open(keymap))["keys"] if k.get("id")}
rects = [keys[i] for i in ids if i in keys]
if len(rects) < 3:
    print("ERROR=宣告的格位在這一層找不到:%s" % ",".join(ids)); sys.exit(0)
# x 用模型(欄的左右邊界是準的),y 一律靠像素 —— 錯的是模型的 y。
x0 = min(r["x"] for r in rects); x1 = max(r["x"] + r["w"] for r in rects)
# ⛔ **不要往 grid_top 上面撈。** 這裡從前寫的是 `grid_top - 6% × 格線區高`
#    ≈ 41 px,那是為了容忍**舊的、算低了 63 px 的 grid_top**(見 read_frame
#    的註解:`FRAME_BOT - GRID_H` 少扣了 honor_bottom_inset)。grid_top 修好
#    之後那 41 px 撈到的是**候選列**:待機時那裡是工具列的地球圖示、組字時
#    是高亮的候選,兩者都落在這一欄的 x 範圍內 —— 於是「左欄變了幾條橫帶」
#    多數出一條,這一關報「三格都被接管」而其實第三格好好的。
#    補償碼跟著錯誤公式長出來,錯誤公式修好之後補償碼就是新的錯誤。
col = (max(0, x0), max(0, grid_top), min(im.width, x1), im.height)
strip, b = im.crop(col), base.crop(col)
sp, bp = strip.load(), b.load()
rowink = [sum(1 for x in range(strip.width) if abs(sp[x, y] - bp[x, y]) > 24)
          for y in range(strip.height)]
floor = max(2, strip.width // 20)
rowink = [n if n >= floor else 0 for n in rowink]
runs, cur = [], None
for y, n in enumerate(rowink):
    if n > 0 and cur is None:
        cur = y
    elif n == 0 and cur is not None:
        runs.append((cur, y)); cur = None
if cur is not None:
    runs.append((cur, strip.height))
merged = []
for r in runs:
    if merged:
        gap = r[0] - merged[-1][1]
        tall = max(r[1] - r[0], merged[-1][1] - merged[-1][0])
        if gap <= max(6, int(tall * 0.6)):
            merged[-1] = (merged[-1][0], r[1]); continue
    merged.append(r)
bands = [r for r in merged if r[1] - r[0] >= 8]
if len(bands) != 2:
    print("ERROR=打 PGM 之後左欄變了 %d 條橫帶,要的是 2 條(qin 與 pin)。"
          "三條 = 三格都被接管、沒有留下沒被接管的那一格;0 條 = 消歧欄根本沒畫"
          % len(bands))
    sys.exit(0)
c0 = col[1] + (bands[0][0] + bands[0][1]) // 2
c1 = col[1] + (bands[1][0] + bands[1][1]) // 2
pitch = c1 - c0
if pitch <= 8 or c1 + pitch >= im.height:
    print("ERROR=兩條橫帶外推不出第三格(帶距 %d px)" % pitch); sys.exit(0)
cy = c1 + pitch
half = int(pitch * 0.4)
cell = (col[0], max(0, cy - half), col[2], min(im.height, cy + half))
if cell[3] - cell[1] < 8:
    print("ERROR=外推出來的第三格高度不合理(%d px)" % (cell[3] - cell[1])); sys.exit(0)
d = ImageChops.difference(im.crop(cell), base.crop(cell)).point(lambda p: 255 if p > 24 else 0)
changed = d.histogram()[255]
area = (cell[2] - cell[0]) * (cell[3] - cell[1])
print("CX=%d CY=%d PITCH=%d CELL=%s CHANGED=%d AREA=%d"
      % ((col[0] + col[2]) // 2, cy, pitch, cell, changed, area))
PY
  }

  # 前面幾關留下的組字與已上屏的字都先清掉。刪除鍵走 librime,多刪幾下沒有副作用。
  for _ in $(seq 1 14); do adbs shell input keyevent 67 >/dev/null 2>&1; done
  sleep 1
  for k in "$KEY_P" "$KEY_G" "$KEY_M"; do
    tap_key "$k" || { fail "$LAYOUT:第 5 關點不到鍵 $k"; continue 2; }
  done
  read_frame || { fail "$LAYOUT:第 5 關讀不到 IME 視窗 frame"; continue; }
  shot "$LOUT/5-pgm.png"
  U5="$(untouched_cell "$LOUT/5-pgm.png" 2>&1)"; RC5=$?
  info "$U5"
  if [ "$RC5" -ne 0 ]; then
    fail "$LAYOUT:第 5 關的裁切這一步自己失敗了(exit $RC5),不是畫面的問題:$U5"
    continue
  fi
  case "$U5" in
    *ERROR=*)
      fail "$LAYOUT:第 5 關量不出「沒被接管的那一格」在哪裡:${U5#*ERROR=}"
      fail "  截圖 $LOUT/5-pgm.png,底圖 $LOUT/0-idle.png"
      continue ;;
  esac
  GATE5_RAN=$((GATE5_RAN + 1))
  CX5="$(printf '%s' "$U5" | sed -n 's/.*CX=\([0-9]*\).*/\1/p')"
  CY5="$(printf '%s' "$U5" | sed -n 's/.*CY=\([0-9]*\).*/\1/p')"
  CH5="$(printf '%s' "$U5" | sed -n 's/.*CHANGED=\([0-9]*\).*/\1/p')"
  AR5="$(printf '%s' "$U5" | sed -n 's/.*AREA=\([0-9]*\).*/\1/p')"
  if [ -z "$CX5" ] || [ -z "$CY5" ] || [ -z "$CH5" ] || [ -z "$AR5" ]; then
    fail "$LAYOUT:第 5 關解析不了量出來的東西(「$U5」)"
    continue
  fi

  # 5a 畫面:那一格在打字前後**逐像素相同**。
  #    正向對照是「前兩格變了」—— untouched_cell 要求剛好兩條變化橫帶,
  #    所以裁歪、底圖拍壞、消歧欄整條沒畫,都會在上面那一步就指名紅掉,
  #    而不會變成這裡的「沒變 = 通過」。
  #    門檻取面積的 1/400:實測乾淨的那一份是 0 個像素(screencap 是決定性的),
  #    留一點給抗鋸齒就夠;鍵面被換成讀音是幾百到幾千個像素,差得很遠。
  if [ "$CH5" -le $((AR5 / 400)) ]; then
    pass "$LAYOUT:沒被接管的那一格($SLOT3_ID)畫的還是原本的「$SLOT3_LABEL」(變了 $CH5/$AR5 px)"
  else
    fail "$LAYOUT:第 5 關 —— 左欄第三格($SLOT3_ID)在組字之後被改動了($CH5/$AR5 個像素)。"
    fail "  沒用到的格位應該**原封不動**照佈局畫。task #78 就是這裡:那一格變成了一個灰色的洞。"
    fail "  截圖 $LOUT/5-pgm.png,底圖 $LOUT/0-idle.png"
  fi

  # 5b 行為:點下去,宿主輸入框真的多出那個標點。
  FT0="$(field_text)"
  adbs shell input tap "$CX5" "$CY5" >/dev/null 2>&1
  sleep 2
  FT1="$(field_text)"
  if [ "$FT1" = "$FT0" ]; then
    fail "$LAYOUT:第 5 關 —— 點下沒被接管的那一格($SLOT3_ID,鍵面「$SLOT3_LABEL」),"
    fail "  宿主輸入框一個字都沒多出來(前後都是「$FT0」)。"
    fail "  那是一顆畫得對、按下去什麼都不做的標點鍵:點擊被導進 onSlot,"
    fail "  而 onSlot 對 Cell.Original 是 Unit(task #78 的形狀,鍵面與幾何完全正常)。"
    fail "  截圖 $LOUT/5-pgm.png"
  elif [ "${FT1%"$SLOT3_LABEL"}" = "$FT1" ]; then
    fail "$LAYOUT:第 5 關 —— 點下 $SLOT3_ID 之後輸入框變成「$FT1」,結尾不是它鍵面上"
    fail "  畫的「$SLOT3_LABEL」—— 鍵面寫的是一件事,送出去的是另一件事。"
    fail "  (這一關假設的是全形標點;真的要開 ascii_punct 的話,這裡要跟著改。)"
  else
    pass "$LAYOUT:點下沒被接管的那一格,宿主輸入框收到了「$SLOT3_LABEL」(「$FT0」→「$FT1」)"
  fi
done

# 收尾的保險:上面每一條 `continue` 都可能跳過迴圈裡那一次移除。
# 留一份單編碼方案在共用模擬器上,下一支腳本會對著它跑而且不知道。
if [ "$SCENARIO" = "stale-schema" ] && [ -x "$ADB" ]; then
  adbs shell "run-as $IME_PKG rm -f files/rime/user/$SCHEMA.custom.yaml" >/dev/null 2>&1 || true
fi
# 第 5 關一次都沒跑到,和它每次都通過,在日誌上長得一模一樣。
# 情境那一條只驗服務層(消歧欄本來就不該出現),不適用這個下界。
#
# ⚠ 帶 --plant 時也不適用,而且不是「順手放寬」:植入本來就會讓前面幾關紅掉、
#   讓那一份佈局 `continue` 掉,於是第 5 關**理所當然**跑不到。把它算成一條
#   FAIL 只會在植入的失敗清單裡多兩條不相干的紅 —— 而這一支的規矩是
#   「紅的必須是指名的那一條」,多出來的紅會讓下一個人以為植入踩到了別的東西。
#   (CI 第一次跑 --plant bad-slot-ids 就正好長這樣:2 條指名的 + 2 條這個。)
#   真正守住 tap-swallowed 的是 plant_expect_re 指名的那句話,不是這個下界。
if [ -z "$SCENARIO" ] && [ -z "$PLANT" ] && [ "$GATE5_RAN" -eq 0 ]; then
  fail "第 5 關一次都沒有跑到 —— 「沒被接管的那一格」在任何一份佈局上都沒驗過。"
  fail "  多半是所有佈局都退化成上方橫排、或第 2/3/4 關先 continue 掉了。"
fi

if [ -n "$SCENARIO" ]; then
  finish "情境 $SCENARIO:方案改寫不了的時候,消歧欄整條沒有出現(斷言的是畫面像素)"
else
  finish "${#T9_LAYOUTS[@]} 份九宮格佈局上都畫出來了、選得下去、單音節也收斂,而且沒被接管的那一格($GATE5_RAN 份佈局上)按下去真的會出標點"
fi
