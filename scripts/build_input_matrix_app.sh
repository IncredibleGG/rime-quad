#!/usr/bin/env bash
#
# build_input_matrix_app.sh — 建立「輸入框型別矩陣」測試靶 APK(不需要 Gradle)
#
# 為什麼要有這支:build_testapp.sh 產生的靶只有一個最單純的 EditText,
# 那是最理想化的目標。真實 app（Telegram、瀏覽器、密碼欄…）的差別在
# `inputType` 與 `InputConnection` 的實作,而那正是 IME bug 的溫床。
# 這支靶把常見的 inputType 攤成一個矩陣,一次一個欄位,方便自動化逐格驗證。
#
# 這個 APK 有三個關鍵設計:
#
#   1. **一次只顯示一個欄位**。用 intent extra 指定要測哪一格:
#          am start -n dev.rime.inputmatrix/.MainActivity --es field textMultiLine
#      欄位固定畫在畫面上方,不會被鍵盤蓋住,也不需要捲動,座標穩定。
#
#   2. **狀態鏡射**。每次內容/選取/組字區變動,都把
#          STATE <field> |<text>| cs=<組字起> ce=<組字迄> sel=<a>,<b>
#      印到 logcat(tag: IMEMATRIX),同時寫進一個 content-desc 為
#      `rime_matrix_mirror` 的 TextView。前者讓自動化跑得快,而且**不受
#      密碼欄遮蔽影響**;後者讓 uiautomator 也讀得到,兩邊互相佐證。
#
#   3. **InputConnection 側錄**。EditText 回傳的 InputConnection 被包了一層
#      InputConnectionWrapper,把 IME 打進來的每一通呼叫
#      (commitText / setComposingText / finishComposingText /
#       deleteSurroundingText / sendKeyEvent / setComposingRegion …)
#      原樣印到 logcat:
#          IC <field> commitText('n',1)
#      這是本靶最重要的能力 —— 不必改 IME 就能看見「IME 到底對宿主做了什麼」,
#      「輸入一個字符卻刪掉一個字符」這種病徵可以直接讀出兇手是哪一通呼叫。
#
# 產出:<專案>/build/inputmatrix/rime-inputmatrix.apk
#   package  : dev.rime.inputmatrix
#   activity : dev.rime.inputmatrix/.MainActivity
#
# 可用的 intent extra:
#   --es field   <id>      要顯示的欄位(見下方 FIELDS;預設 text)
#   --es prefill <str>     預先填入的內容
#   --ei cursor  <n>       預填後把游標放在第 n 個字之後(預設放最後)
#
# 欄位 id:
#   text  textMultiLine  textCapSentencesMultiLine  textNoSuggestions
#   textUri  textEmailAddress  textPassword  textVisiblePassword  number
#   webview   (WebView 裡的 <input type="text">)
#
# 注意:本腳本刻意**不修改** scripts/build_testapp.sh(那支是別人的)。

set -euo pipefail

# ⛔ **唯讀出口。** `scripts/verify_script_readonly.sh` 把每一支腳本的
#   `--help` 跑一遍,而且用 shim 檢查它有沒有碰外部工具。這一支從前沒有
#   `--help`,於是 `--help` 被當成一般啟動 —— 一路跑下去(建置／git／推檔)。
#   說明不得有任何副作用。
case "${1:-}" in
  -h|--help)
    sed -n '2,/^set -[eu]/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0 ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
BUILD_TOOLS_VER="${RIME_BUILD_TOOLS:-35.0.0}"
API_LEVEL="${RIME_API_LEVEL:-35}"

BT="$ANDROID_SDK_ROOT/build-tools/$BUILD_TOOLS_VER"
ANDROID_JAR="$ANDROID_SDK_ROOT/platforms/android-$API_LEVEL/android.jar"

OUT_DIR="$PROJECT_ROOT/build/inputmatrix"
WORK="$OUT_DIR/work"
APK="$OUT_DIR/rime-inputmatrix.apk"
KEYSTORE="$OUT_DIR/debug.keystore"

die() { echo "[build_input_matrix_app] 錯誤: $*" >&2; exit 1; }
info() { echo "[build_input_matrix_app] $*" >&2; }

[ -d "$BT" ]          || die "找不到 build-tools $BUILD_TOOLS_VER:$BT"
[ -f "$ANDROID_JAR" ] || die "找不到 android.jar:$ANDROID_JAR"
command -v javac   >/dev/null || die "找不到 javac(需要 JDK 17)"
command -v keytool  >/dev/null || die "找不到 keytool"

rm -rf "$WORK"
mkdir -p "$WORK/src/dev/rime/inputmatrix" "$WORK/classes" "$WORK/dex"

# ------------------------------------------------------------ 產生原始碼 ---

cat > "$WORK/AndroidManifest.xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="dev.rime.inputmatrix"
    android:versionCode="1"
    android:versionName="1.0">
    <uses-sdk android:minSdkVersion="24" android:targetSdkVersion="35" />
    <application android:label="RIME Input Matrix">
        <activity android:name=".MainActivity"
                  android:exported="true"
                  android:launchMode="singleTop"
                  android:windowSoftInputMode="stateAlwaysVisible|adjustResize">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
XML

cat > "$WORK/src/dev/rime/inputmatrix/LoggingInputConnection.java" <<'JAVA'
package dev.rime.inputmatrix;

import android.util.Log;
import android.view.KeyEvent;
import android.view.inputmethod.CompletionInfo;
import android.view.inputmethod.CorrectionInfo;
import android.view.inputmethod.ExtractedText;
import android.view.inputmethod.ExtractedTextRequest;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputConnectionWrapper;

/**
 * 把 IME 打進來的每一通 InputConnection 呼叫原樣記錄下來。
 *
 * 這一層的存在理由:診斷「打一個字卻刪一個字」這類病徵時,唯一能一刀切開
 * 「IME 送錯了」與「宿主處理錯了」的證據,就是 IME 實際呼叫了哪些方法、
 * 帶什麼參數。把它做在**測試靶**而不是 IME 裡,好處是被測的 IME 完全不必
 * 為了測試而改動 —— 連正式發布的 APK 都能這樣側錄。
 */
public class LoggingInputConnection extends InputConnectionWrapper {

    private final String field;

    public LoggingInputConnection(InputConnection target, String field) {
        super(target, false);
        this.field = field;
    }

    private void note(String what) {
        Log.i(MainActivity.TAG, "IC " + field + " " + what);
    }

    private static String q(CharSequence s) {
        if (s == null) return "null";
        StringBuilder b = new StringBuilder("'");
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\n') b.append("\\n");
            else if (c == '\r') b.append("\\r");
            else b.append(c);
        }
        return b.append('\'').toString();
    }

    @Override
    public boolean commitText(CharSequence text, int newCursorPosition) {
        note("commitText(" + q(text) + "," + newCursorPosition + ")");
        return super.commitText(text, newCursorPosition);
    }

    @Override
    public boolean setComposingText(CharSequence text, int newCursorPosition) {
        note("setComposingText(" + q(text) + "," + newCursorPosition + ")");
        return super.setComposingText(text, newCursorPosition);
    }

    @Override
    public boolean setComposingRegion(int start, int end) {
        note("setComposingRegion(" + start + "," + end + ")");
        return super.setComposingRegion(start, end);
    }

    @Override
    public boolean finishComposingText() {
        note("finishComposingText()");
        return super.finishComposingText();
    }

    @Override
    public boolean deleteSurroundingText(int before, int after) {
        note("deleteSurroundingText(" + before + "," + after + ")");
        return super.deleteSurroundingText(before, after);
    }

    @Override
    public boolean deleteSurroundingTextInCodePoints(int before, int after) {
        note("deleteSurroundingTextInCodePoints(" + before + "," + after + ")");
        return super.deleteSurroundingTextInCodePoints(before, after);
    }

    @Override
    public boolean sendKeyEvent(KeyEvent event) {
        note("sendKeyEvent(action=" + event.getAction() + ",code=" + event.getKeyCode() + ")");
        return super.sendKeyEvent(event);
    }

    @Override
    public boolean performEditorAction(int actionCode) {
        note("performEditorAction(" + actionCode + ")");
        return super.performEditorAction(actionCode);
    }

    @Override
    public boolean commitCompletion(CompletionInfo info) {
        note("commitCompletion()");
        return super.commitCompletion(info);
    }

    @Override
    public boolean commitCorrection(CorrectionInfo info) {
        note("commitCorrection()");
        return super.commitCorrection(info);
    }

    @Override
    public boolean setSelection(int start, int end) {
        note("setSelection(" + start + "," + end + ")");
        return super.setSelection(start, end);
    }

    @Override
    public ExtractedText getExtractedText(ExtractedTextRequest request, int flags) {
        note("getExtractedText(flags=" + flags + ")");
        return super.getExtractedText(request, flags);
    }

    @Override
    public CharSequence getTextBeforeCursor(int n, int flags) {
        CharSequence r = super.getTextBeforeCursor(n, flags);
        note("getTextBeforeCursor(" + n + ") -> " + q(r));
        return r;
    }

    @Override
    public boolean beginBatchEdit() {
        note("beginBatchEdit()");
        return super.beginBatchEdit();
    }

    @Override
    public boolean endBatchEdit() {
        note("endBatchEdit()");
        return super.endBatchEdit();
    }
}
JAVA

cat > "$WORK/src/dev/rime/inputmatrix/ProbeEditText.java" <<'JAVA'
package dev.rime.inputmatrix;

import android.content.Context;
import android.util.Log;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.widget.EditText;

/** 會把自己的 EditorInfo 與 IME 呼叫記錄下來的 EditText。 */
public class ProbeEditText extends EditText {

    private String field = "?";

    public ProbeEditText(Context c) { super(c); }

    public void setFieldName(String f) { this.field = f; }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
        InputConnection ic = super.onCreateInputConnection(outAttrs);
        Log.i(MainActivity.TAG, "EDITORINFO " + field
                + " inputType=0x" + Integer.toHexString(outAttrs.inputType)
                + " imeOptions=0x" + Integer.toHexString(outAttrs.imeOptions)
                + " initialSelStart=" + outAttrs.initialSelStart
                + " initialSelEnd=" + outAttrs.initialSelEnd);
        if (ic == null) {
            Log.w(MainActivity.TAG, "EDITORINFO " + field + " onCreateInputConnection 回傳 null");
            return null;
        }
        return new LoggingInputConnection(ic, field);
    }
}
JAVA

cat > "$WORK/src/dev/rime/inputmatrix/MainActivity.java" <<'JAVA'
package dev.rime.inputmatrix;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputType;
import android.text.Selection;
import android.text.Spannable;
import android.text.TextWatcher;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.InputMethodManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * 輸入框型別矩陣測試靶。
 *
 * 一次只顯示一個欄位(由 intent extra `field` 指定),欄位固定畫在畫面上方,
 * 所以座標穩定、不會被鍵盤蓋住、不需要捲動。這是為了讓自動化能可靠地
 * 「逐鍵點 → 讀回內容」而做的取捨:比起把十個框塞在一個可捲動的清單裡,
 * 一次一格的重跑成本低很多,失敗時也不會分不清是捲動還是輸入出的錯。
 */
public class MainActivity extends Activity {

    public static final String TAG = "IMEMATRIX";

    /** 自動化定位用的 content-description。 */
    public static final String INPUT_DESC = "rime_matrix_input";
    public static final String MIRROR_DESC = "rime_matrix_mirror";

    private String field = "text";
    private ProbeEditText edit;
    private WebView web;
    private TextView mirror;

    /** 欄位 id → inputType。null 代表不是 EditText(webview)。 */
    static Integer inputTypeOf(String id) {
        if ("text".equals(id)) {
            return InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_NORMAL;
        }
        if ("textMultiLine".equals(id)) {
            return InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE;
        }
        if ("textCapSentencesMultiLine".equals(id)) {
            // Telegram 的訊息輸入框就是這一類。
            return InputType.TYPE_CLASS_TEXT
                    | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
                    | InputType.TYPE_TEXT_FLAG_MULTI_LINE;
        }
        if ("textNoSuggestions".equals(id)) {
            return InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        }
        if ("textUri".equals(id)) {
            return InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI;
        }
        if ("textEmailAddress".equals(id)) {
            return InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS;
        }
        if ("textPassword".equals(id)) {
            return InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD;
        }
        if ("textVisiblePassword".equals(id)) {
            return InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD;
        }
        if ("number".equals(id)) {
            return InputType.TYPE_CLASS_NUMBER;
        }
        return null;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        build(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        build(intent);
    }

    private void build(Intent intent) {
        String f = intent != null ? intent.getStringExtra("field") : null;
        field = (f == null || f.length() == 0) ? "text" : f;
        String prefill = intent != null ? intent.getStringExtra("prefill") : null;
        int cursor = intent != null ? intent.getIntExtra("cursor", -1) : -1;

        float d = getResources().getDisplayMetrics().density;
        int density = (int) d;
        int pad = (int) (12 * d);
        // ActionBar 會蓋在 content 上,頂端要讓出足夠空間,否則輸入框點不到。
        int padTop = (int) (130 * d);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.TOP);
        root.setPadding(pad, padTop, pad, pad);
        root.setBackgroundColor(Color.WHITE);

        TextView title = new TextView(this);
        title.setText("FIELD: " + field);
        title.setTextSize(18);
        title.setTextColor(Color.BLACK);
        root.addView(title);

        edit = null;
        web = null;

        Integer type = inputTypeOf(field);
        if (type != null) {
            edit = new ProbeEditText(this);
            edit.setFieldName(field);
            edit.setId(1001);
            edit.setContentDescription(INPUT_DESC);
            edit.setHint("type here");
            edit.setTextSize(26);
            edit.setTextColor(Color.BLACK);
            edit.setInputType(type);
            // 多行欄位要能長高,但固定最少三行高,座標才穩。
            edit.setMinLines(1);
            edit.setMaxLines(3);
            root.addView(edit, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT));
            edit.addTextChangedListener(new TextWatcher() {
                public void beforeTextChanged(CharSequence s, int a, int b, int c) { }
                public void onTextChanged(CharSequence s, int a, int b, int c) { }
                public void afterTextChanged(Editable s) { report(); }
            });
            if (prefill != null) {
                edit.setText(prefill);
                final int at =
                        (cursor >= 0 && cursor <= prefill.length()) ? cursor : prefill.length();
                // ⚠ 必須 post。EditText 取得焦點時會自己把游標移到文字尾端,
                //    在 requestFocus() 之前設的選取範圍會被蓋掉 —— 「游標在
                //    中間」的情境於是變成「游標在結尾」,測了個寂寞。
                edit.post(new Runnable() {
                    public void run() {
                        Selection.setSelection(edit.getText(), at);
                    }
                });
            }
        } else if ("webview".equals(field)) {
            web = new WebView(this);
            web.setContentDescription(INPUT_DESC);
            web.setFocusable(true);
            web.setFocusableInTouchMode(true);
            WebSettings s = web.getSettings();
            s.setJavaScriptEnabled(true);
            web.addJavascriptInterface(new Bridge(), "RimeProbe");
            String value = prefill == null ? "" : prefill;
            String html = "<html><head><meta name='viewport' "
                    + "content='width=device-width,initial-scale=1'></head>"
                    + "<body style='margin:0;padding:8px;font-size:26px'>"
                    + "<input id='t' type='text' style='width:96%;font-size:26px' value='"
                    + value.replace("'", "") + "'>"
                    + "<script>"
                    + "var t=document.getElementById('t');"
                    + "function push(){RimeProbe.report(t.value);}"
                    + "t.addEventListener('input',push);"
                    + "t.addEventListener('compositionupdate',push);"
                    + "t.addEventListener('compositionend',push);"
                    + "window.onload=function(){t.focus();push();};"
                    + "</script></body></html>";
            web.loadDataWithBaseURL("about:blank", html, "text/html", "utf-8", null);
            root.addView(web, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, 40 * density));
        } else {
            TextView bad = new TextView(this);
            bad.setText("UNKNOWN FIELD: " + field);
            bad.setTextColor(Color.RED);
            root.addView(bad);
        }

        mirror = new TextView(this);
        mirror.setContentDescription(MIRROR_DESC);
        mirror.setTextSize(16);
        mirror.setTextColor(Color.rgb(0, 96, 0));
        mirror.setText("STATE " + field + " || cs=-1 ce=-1 sel=0,0");
        root.addView(mirror);

        setContentView(root);

        getWindow().setSoftInputMode(
                WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE
                        | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);

        final View focusTarget = (edit != null) ? edit : (web != null ? web : null);
        if (focusTarget != null) {
            focusTarget.requestFocus();
            focusTarget.post(new Runnable() {
                public void run() {
                    InputMethodManager imm = (InputMethodManager)
                            getSystemService(Context.INPUT_METHOD_SERVICE);
                    if (imm != null) {
                        imm.showSoftInput(focusTarget, InputMethodManager.SHOW_IMPLICIT);
                    }
                }
            });
        }
        Log.i(TAG, "READY " + field);
        report();
    }

    /** WebView 裡的 <input> 把值回報進來,好讓 native 這一側也能鏡射。 */
    private class Bridge {
        @JavascriptInterface
        public void report(final String value) {
            runOnUiThread(new Runnable() {
                public void run() { publish(value, -1, -1, -1, -1); }
            });
        }
    }

    private void report() {
        if (edit == null) { return; }
        Editable e = edit.getText();
        int cs = BaseInputConnection.getComposingSpanStart(e);
        int ce = BaseInputConnection.getComposingSpanEnd(e);
        publish(e.toString(), cs, ce, Selection.getSelectionStart(e), Selection.getSelectionEnd(e));
    }

    private void publish(String text, int cs, int ce, int selA, int selB) {
        String flat = text.replace("\n", "\\n").replace("\r", "");
        String line = "STATE " + field + " |" + flat + "| cs=" + cs + " ce=" + ce
                + " sel=" + selA + "," + selB + " len=" + text.length();
        Log.i(TAG, line);
        if (mirror != null) { mirror.setText(line); }
    }
}
JAVA

# ---------------------------------------------------------------- 編譯 ---

info "aapt2 link …"
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
  "$WORK/src/dev/rime/inputmatrix/"*.java 2>&1 | grep -v "bootstrap class path" || true

[ -f "$WORK/classes/dev/rime/inputmatrix/MainActivity.class" ] || die "javac 沒有產生 class 檔"

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
