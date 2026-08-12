package org.luminakey.ime.core

import android.view.inputmethod.EditorInfo

/**
 * 兩個「宿主編輯框說了什麼、我們該怎麼辦」的判斷。
 *
 * 抽成純函式的理由與本專案其他幾支相同：`InputMethodService` 的回呼只有真的
 * 跑起一個輸入法才進得去，而這兩個判斷本身與 Android 無關 —— 它們是規則，
 * 規則要測得到。服務那一側只剩「呼叫它、照結果做」。
 */
object HostEditorPolicy {

    /* ───────────────────── 一、全螢幕 extract 模式 ───────────────────── */

    /**
     * 這個編輯框要不要進 AOSP 的全螢幕 extract 模式。**永遠不要。**
     *
     * ── 不覆寫的話會發生什麼（實測，emulator-5558，橫屏）────────────────
     * `InputMethodService.onEvaluateFullscreenMode()` 的預設實作是
     * 「螢幕高度不夠就進全螢幕」，橫屏幾乎必然成立。那時系統會在鍵盤上方畫出
     * 一整條**它自己的** UI：一個 `ExtractEditText` 加一顆原生的 `Done` 按鈕，
     * 用的是系統主題的漸層底色。畫面上於是同時有兩個輸入框、兩套外觀，
     * 而上面那一個**不是我們畫的**，也吃不到 `core/themes` 的任何設定。
     *
     * 使用者看到的就是「轉個螢幕，鍵盤上面多出一條不屬於這個 app 的東西」。
     *
     * ── 為什麼是「一律 false」而不是「橫屏才 false」──────────────────────
     * extract 模式的存在理由是「宿主的編輯框被鍵盤蓋住了，所以另外畫一個」。
     * 本專案已經在 `onComputeInsets` 那一輪處理過遮擋（見 task #49）：宿主
     * 只要正常消費 insets 就看得到自己的框。既然遮擋不是靠 extract 解決的，
     * extract 就只剩下副作用。
     *
     * ── 參數為什麼留著 ──────────────────────────────────────────────────
     * `IME_FLAG_NO_FULLSCREEN` 是宿主**明說**「不要全螢幕」。我們的答案本來
     * 就是 false，所以這個旗標是自動被尊重的 —— 但把它寫進簽章與測試裡，
     * 是為了讓「哪天有人把這裡改成看螢幕高度」這件事當場被抓到：那時
     * 帶著旗標的那一條斷言會紅。
     */
    fun useFullscreen(imeOptions: Int): Boolean = false

    /** 宿主明說不要全螢幕（`EditorInfo.IME_FLAG_NO_FULLSCREEN`）。 */
    fun hostForbidsFullscreen(imeOptions: Int): Boolean =
        (imeOptions and EditorInfo.IME_FLAG_NO_FULLSCREEN) != 0

    /* ───────────────────── 二、游標被移走了 ───────────────────── */

    /**
     * 宿主回報了一次新的游標位置 —— 那代表「使用者把游標移走了」嗎？
     *
     * ── 為什麼需要它 ────────────────────────────────────────────────────
     * 在這段之前，`onUpdateSelection()` 在整個 repo 裡**一次都沒有出現過**。
     * 也就是說：使用者在組字途中用手指點到句子中間、或選取了一段字，
     * librime 完全不知情 —— 它仍然以為自己在組那一串，下一次按鍵會把
     * preedit 畫到一個與原本完全無關的位置上。
     *
     * ── 判準 ────────────────────────────────────────────────────────────
     * 我們每次都用 `setComposingText(text, 1)` 把游標放在組字區的**末端**。
     * 所以「游標還在我們放的地方」= 沒有選取範圍，而且位置落在宿主回報的
     * 組字區之內。落在外面、或出現選取範圍，就是使用者自己動過。
     *
     * ⚠ **宿主回報 `candidatesStart < 0`（沒有組字區）時一律回 false。**
     * 那看起來像「宿主把組字區收掉了，引擎卻還在組字」，很想當成要清掉；
     * 但它同時也是**我們自己剛送出的那一次操作的過期座標**：
     * `refreshFromRime` 會先 `commitText()` 再 `setComposingText()`，兩者之間
     * 宿主真的沒有組字區，而那一刻的回呼會晚一點才送到我們手上。把它當成
     * 「使用者移走了游標」的話，每一次選字都會順手把接下來那一段組字清掉 ——
     * 比原本的缺陷嚴重得多。真正的「使用者移走游標」一定伴隨一個**有效的**
     * 組字區（宿主要到 `finishComposingText()` 才會收掉它），所以這條路走
     * 有效座標那一支就夠了。
     */
    fun cursorLeftComposition(
        engineComposing: Boolean,
        newSelStart: Int,
        newSelEnd: Int,
        candidatesStart: Int,
        candidatesEnd: Int,
    ): Boolean {
        if (!engineComposing) return false
        if (candidatesStart < 0 || candidatesEnd < 0) return false
        if (newSelStart != newSelEnd) return true
        return newSelStart < candidatesStart || newSelStart > candidatesEnd
    }
}
