#!/usr/bin/env bash
#
# build_testapp.sh — 建立一個極小的「輸入測試靶」APK(不需要 Gradle)
#
# 這個 APK 只有一個 Activity,畫面上是一個 EditText,開啟時強制彈出軟鍵盤。
# 用途是給 verify_ime.sh 當作穩定、與 Google 內建 App 無關的輸入目標。
#
# 直接用 build-tools 的 aapt2 / d8 / zipalign / apksigner 組出來,
# 所以不需要 Gradle、不需要下載任何相依套件、離線也能建。
#
# 產出:<專案>/build/imetest/rime-imetest.apk
#   package  : dev.rime.imetest
#   activity : dev.rime.imetest/.MainActivity
#   輸入框的 content-desc 為 rime_test_input
#
# ── 「畫面下方」那個輸入框(--ez bottom true)──────────────────────────────
#
# 預設的靶欄位在**畫面最上面**。那個位置有一個測不到的東西:鍵盤有沒有把宿主
# 的內容蓋掉。最上面的框不管鍵盤佔多少空間都看得見,所以「鍵盤蓋住輸入框」
# 這一整類缺陷在這支測試靶上是隱形的 —— 而那正是使用者最常遇到的形態
# (聊天室、搜尋列、表單的最後一欄都在畫面下緣)。
#
# 所以多一個**選用**的下緣欄位:
#
#   am start -n dev.rime.imetest/.MainActivity --ez bottom true
#     → 版面變成「標題 + 上方框 + 彈性空白 + 下緣框」,焦點落在下緣框,
#       content-desc 為 rime_test_input_bottom。
#
#   --ez insets false
#     → **不**安裝 WindowInsets 監聽器。給 scripts/verify_insets.sh 當反向對照:
#       這樣下緣框一定會被鍵盤蓋住,用來證明那支檢查在該紅的時候會紅。
#
# ⚠ **`android:windowSoftInputMode="adjustResize"` 在這裡是沒有作用的,不要拿它
#   當判準。** 這一點花了一輪才查清楚:`SOFT_INPUT_ADJUST_RESIZE` 自 API 30 起
#   已棄用,而 targetSdk 35 又強制 edge-to-edge,於是這個 Activity 不但輸入框
#   不會被推上去,連標題都畫到狀態列底下。**換成 Gboard 一模一樣** ——
#   所以「下緣框被蓋住」單獨看完全不能證明輸入法有問題,那是宿主自己沒有
#   消費 insets。要驗輸入法,宿主必須先是個正常的宿主:
#
#     root.setOnApplyWindowInsetsListener(... getInsets(Type.ime() | Type.systemBars()) ...)
#
#   下緣模式因此一律安裝這個監聽器,並把量到的 ime bottom 寫進標題的
#   content-desc(`ime_inset_bottom=<px>`),讓自動化讀得到一個數字而不是猜像素。
#
# ⚠ 不帶 extra 時的行為與加這段之前**完全相同**(下緣框根本不會被加進
#   view 樹,監聽器也不會裝)。verify_ime.sh / verify_rime_compose.sh /
#   verify_longpress.sh 都靠這個靶,它們掃的是畫面下半部,
#   多一個看不見的框會讓它們戳錯東西。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
BUILD_TOOLS_VER="${RIME_BUILD_TOOLS:-35.0.0}"
API_LEVEL="${RIME_API_LEVEL:-35}"

BT="$ANDROID_SDK_ROOT/build-tools/$BUILD_TOOLS_VER"
ANDROID_JAR="$ANDROID_SDK_ROOT/platforms/android-$API_LEVEL/android.jar"

OUT_DIR="$PROJECT_ROOT/build/imetest"
WORK="$OUT_DIR/work"
APK="$OUT_DIR/rime-imetest.apk"
KEYSTORE="$OUT_DIR/debug.keystore"

die() { echo "[build_testapp] 錯誤: $*" >&2; exit 1; }
info() { echo "[build_testapp] $*" >&2; }

[ -d "$BT" ]          || die "找不到 build-tools $BUILD_TOOLS_VER:$BT"
[ -f "$ANDROID_JAR" ] || die "找不到 android.jar:$ANDROID_JAR"
command -v javac  >/dev/null || die "找不到 javac(需要 JDK 17)"
command -v keytool >/dev/null || die "找不到 keytool"

rm -rf "$WORK"
mkdir -p "$WORK/src/dev/rime/imetest" "$WORK/classes" "$WORK/dex"

# ------------------------------------------------------------ 產生原始碼 ---

cat > "$WORK/AndroidManifest.xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="dev.rime.imetest"
    android:versionCode="1"
    android:versionName="1.0">
    <uses-sdk android:minSdkVersion="24" android:targetSdkVersion="35" />
    <application android:label="RIME IME Test">
        <activity android:name=".MainActivity"
                  android:exported="true"
                  android:windowSoftInputMode="stateAlwaysVisible|adjustResize">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
XML

cat > "$WORK/src/dev/rime/imetest/MainActivity.java" <<'JAVA'
package dev.rime.imetest;

import android.app.Activity;
import android.content.Context;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * 只做一件事:顯示一個 EditText 並強制彈出軟鍵盤。
 * 給自動化測試(verify_ime.sh)當輸入目標用。
 *
 * 選用的第二個框(intent extra `bottom`)貼在畫面下緣,用來驗「鍵盤有沒有把
 * 宿主的內容蓋掉」—— 上方那個框無論鍵盤佔多少空間都看得見,測不到這件事。
 * 詳見本檔頭的說明。
 */
public class MainActivity extends Activity {

    /** 自動化用來定位輸入框的 content-description。 */
    public static final String INPUT_DESC = "rime_test_input";
    /** 貼在畫面下緣的那個框(只有 `--ez bottom true` 時才存在)。 */
    public static final String BOTTOM_DESC = "rime_test_input_bottom";
    private EditText mInput;
    /** 標題,兼作「宿主收到的 ime inset」的看板(寫在 content-desc 上)。 */
    private TextView mTitle;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        boolean bottom = getIntent() != null
                && getIntent().getBooleanExtra("bottom", false);
        // 反向對照用:關掉 insets 消費,下緣框就會被鍵盤蓋住。
        boolean useInsets = getIntent() == null
                || getIntent().getBooleanExtra("insets", true);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.TOP);
        final int pad = (int) (16 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad * 4, pad, pad);

        TextView title = new TextView(this);
        title.setText("RIME IME Test Target");
        title.setTextSize(20);
        root.addView(title);
        mTitle = title;

        EditText input = new EditText(this);
        input.setContentDescription(INPUT_DESC);
        input.setHint("type here");
        input.setTextSize(24);
        input.setSingleLine(false);
        root.addView(input, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        // 下緣框:一個 weight=1 的空白把它推到畫面最底。
        // 只有明確要求時才加進 view 樹 —— 既有的驗證腳本會掃畫面下半部,
        // 多一個它們不知道的框會讓它們戳錯東西。
        EditText bottomInput = null;
        if (bottom) {
            View spacer = new View(this);
            LinearLayout.LayoutParams sp = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f);
            root.addView(spacer, sp);

            bottomInput = new EditText(this);
            bottomInput.setContentDescription(BOTTOM_DESC);
            bottomInput.setHint("bottom field");
            bottomInput.setTextSize(24);
            bottomInput.setSingleLine(false);
            root.addView(bottomInput, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT));
        }

        setContentView(root);

        getWindow().setSoftInputMode(
                WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE
                        | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);

        // 下緣模式才裝 insets 監聽器。**這一段才是真正讓「鍵盤有沒有蓋住內容」
        // 有意義的東西** —— 沒有它,這個 Activity 對 Gboard 與對本專案的輸入法
        // 一樣會被蓋住(實測過),那個結果不能拿來指控任何一方。
        if (bottom && useInsets && android.os.Build.VERSION.SDK_INT >= 30) {
            root.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
                @Override
                public WindowInsets onApplyWindowInsets(View v, WindowInsets insets) {
                    android.graphics.Insets bars =
                            insets.getInsets(WindowInsets.Type.systemBars());
                    android.graphics.Insets ime =
                            insets.getInsets(WindowInsets.Type.ime());
                    int bottomPad = Math.max(bars.bottom, ime.bottom);
                    v.setPadding(v.getPaddingLeft(), bars.top + pad,
                            v.getPaddingRight(), bottomPad);
                    // 量到的數字給自動化讀。讀數字比比像素可靠,而且失敗訊息
                    // 能直接說出「宿主收到的 ime inset 是 0」。
                    mTitle.setContentDescription("ime_inset_bottom=" + ime.bottom);
                    return insets;
                }
            });
            root.requestApplyInsets();
        }

        mInput = bottomInput != null ? bottomInput : input;
        mInput.requestFocus();
        askForKeyboard();
    }

    // 為什麼要重試,而不是在 onCreate 裡叫一次就好:
    //
    //   onCreate 裡的 showSoftInput() 可能跑在視窗拿到焦點之前。那時 IMM 還沒有
    //   「served view」,請求會被丟掉 —— logcat 裡是
    //     ImeTracker: onFailed at PHASE_CLIENT_VIEW_SERVED
    //   而且**完全沒有其他徵兆**:輸入法進程根本不會被啟動,看起來就像輸入法壞了。
    //
    //   這是個競態,所以它時好時壞。CI 上同一個 commit 一次過、一次沒過,
    //   查了才知道是這支測試靶自己沒把鍵盤叫起來,不是輸入法的問題。
    private void askForKeyboard() {
        if (mInput == null) return;
        InputMethodManager imm =
                (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm == null) return;
        mInput.requestFocus();
        imm.showSoftInput(mInput, InputMethodManager.SHOW_IMPLICIT);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus) return;
        // 拿到焦點之後才是真正有效的那一次。再補幾次,涵蓋首次部署把 IME
        // 進程拖慢、系統把請求丟掉的情況。
        askForKeyboard();
        for (int i = 1; i <= 6; i++) {
            mInput.postDelayed(new Runnable() {
                @Override public void run() { askForKeyboard(); }
            }, i * 2000L);
        }
    }
}
JAVA

# ---------------------------------------------------------------- 編譯 ---

info "aapt2 link(產生 resources.arsc 與 manifest)…"
"$BT/aapt2" link \
  -I "$ANDROID_JAR" \
  --manifest "$WORK/AndroidManifest.xml" \
  --min-sdk-version 24 \
  --target-sdk-version 35 \
  -o "$WORK/base.apk"

info "javac …"
javac -nowarn -source 8 -target 8 \
  -classpath "$ANDROID_JAR" \
  -d "$WORK/classes" \
  "$WORK/src/dev/rime/imetest/MainActivity.java" 2>&1 | grep -v "bootstrap class path" || true

[ -f "$WORK/classes/dev/rime/imetest/MainActivity.class" ] || die "javac 沒有產生 class 檔"

info "d8 …"
"$BT/d8" --lib "$ANDROID_JAR" --min-api 24 \
  --output "$WORK/dex" \
  $(find "$WORK/classes" -name '*.class')

info "打包 classes.dex 進 apk …"
( cd "$WORK/dex" && zip -q "$WORK/base.apk" classes.dex )

info "zipalign …"
"$BT/zipalign" -f -p 4 "$WORK/base.apk" "$WORK/aligned.apk"

if [ ! -f "$KEYSTORE" ]; then
  info "產生 debug keystore …"
  keytool -genkeypair -v \
    -keystore "$KEYSTORE" -storepass android -keypass android \
    -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=RIME Debug, OU=Test, O=RIME, L=NA, S=NA, C=TW" >/dev/null 2>&1
fi

info "apksigner …"
"$BT/apksigner" sign \
  --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
  --ks-key-alias androiddebugkey \
  --out "$APK" "$WORK/aligned.apk"

"$BT/apksigner" verify "$APK" >/dev/null

rm -rf "$WORK"
info "完成:$APK ($(wc -c < "$APK") bytes)"
echo "$APK"
