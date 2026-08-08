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
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * 只做一件事:顯示一個 EditText 並強制彈出軟鍵盤。
 * 給自動化測試(verify_ime.sh)當輸入目標用。
 */
public class MainActivity extends Activity {

    /** 自動化用來定位輸入框的 content-description。 */
    public static final String INPUT_DESC = "rime_test_input";
    private EditText mInput;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.TOP);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad * 4, pad, pad);

        TextView title = new TextView(this);
        title.setText("RIME IME Test Target");
        title.setTextSize(20);
        root.addView(title);

        EditText input = new EditText(this);
        input.setContentDescription(INPUT_DESC);
        input.setHint("type here");
        input.setTextSize(24);
        input.setSingleLine(false);
        root.addView(input, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        setContentView(root);

        getWindow().setSoftInputMode(
                WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE
                        | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);

        input.requestFocus();
        mInput = input;
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
