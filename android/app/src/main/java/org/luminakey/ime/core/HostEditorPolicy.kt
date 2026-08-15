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
     * 這個編輯框要不要進 AOSP 的全螢幕 extract 模式。
     * **答案就是 AOSP 的答案：橫屏要，除非宿主明說不要。**
     *
     * ── 上一版做錯了什麼（這一條是回頭修的）────────────────────────────
     * 上一版在這裡一律回 `false`，理由是「橫屏時系統會在鍵盤上方畫一條
     * 不是我們畫的 UI，那條吃不到 core/themes」。原始的抱怨是對的，但拿掉
     * 整個模式不是修法 —— **extract 模式存在的理由就是橫屏螢幕不夠高**。
     *
     * 實測（emulator-5558，1080×2400 @420dpi，轉橫屏 → 2400×1080）：
     * 鍵盤視窗的 frame 是 `[128,229][2400,1080]`，也就是 851 px = 螢幕高的
     * **79 %**。宿主 `dev.rime.imetest` 縮排之後只剩 189 px，那 189 px 全被
     * 它自己的標題列佔滿：`uiautomator dump` 裡**一個輸入框節點都不存在**。
     * 使用者看不到自己正在打什麼 —— 比那條原生輸入條嚴重。
     *
     * 所以這一版換一條路：**讓 extract 模式生效，但那一條由我們自己畫**
     * （見 `keyboard/ThemedExtractView`，經 `onCreateExtractTextView()` 交給
     * 系統的 `setExtractView()`）。G46 抱怨的「吃不到主題」被修掉，而
     * 「橫屏看得見自己打的字」這個補償留著。
     *
     * ── 為什麼判準與 AOSP 一模一樣 ────────────────────────────────────
     * `InputMethodService.onEvaluateFullscreenMode()` 的預設實作就是
     * 「橫屏 → true，但宿主帶了 `IME_FLAG_NO_FULLSCREEN` → false」。
     * 兩個分支都留得住「使用者看得到自己打的字」：
     *
     *   · 走 extract：字畫在我們的 [ThemedExtractView] 上。
     *   · 宿主說 `IME_FLAG_NO_FULLSCREEN`：宿主自己宣告它會照 ime inset
     *     排版（`verify_insets.sh` 驗的就是那條路），我們不該替它改主意。
     *
     * 至於 `IME_FLAG_NO_EXTRACT_UI`：AOSP 仍然進全螢幕、只是把 extract 那一條
     * 藏起來，於是鍵盤以上是透明的、**宿主沒有被縮排**因此仍然看得到自己。
     * 那也是一條「看得見」的路，所以這裡不另外處理，交給
     * `onUpdateExtractingVisibility()` 的預設實作。
     *
     * ⚠ 有一件 AOSP 做得到而我們做不到：它還會看
     *   `EditorInfo.internalImeOptions` 的 `APP_WINDOW_PORTRAIT`（分割視窗／
     *   桌面模式下，顯示器是橫的而宿主視窗是直的）。那個欄位是 `@hide`，
     *   應用程式讀不到。所以在那種視窗形狀下我們會多進一次全螢幕。
     *   已知殘留，記在 docs/product-gaps.md。
     *
     * ── 為什麼還要看尺寸（這一段是後來補的）────────────────────────────
     * 上面那條規則挑「進全螢幕」的理由是一個**手機上的量測**（851/1080 = 79 %，
     * 宿主排不下自己的輸入框）。在大螢幕上那個前提**不成立**，而規則裡沒有
     * 任何一項在看尺寸 —— 於是平板／摺疊機展開後照樣進全螢幕。
     *
     * 實測（emulator-5558，`wm size 1600x2560` + `wm density 240`，轉橫屏
     * → 2560×1600 px @1.5 = 1706×1066 dp）：
     *   · `mFullscreenApplied=true mIsFullscreen=true` —— 照樣進。
     *   · 鍵盤視窗要的高度只有 506 px = 337 dp，**佔螢幕高的 31 %**。
     *   · 宿主被整片換掉，畫面中間留下約 700 dp 的空白。
     *     截圖：`build/ship-evidence/before-tablet-land.png`
     *
     * 也就是說：宿主明明排得下，我們卻把它整個換成一條 extract 輸入條。
     *
     * AOSP 自家的 LatinIME 是用 `config_use_fullscreen_mode` 這個**隨螢幕大小
     * 變**的資源在擋。這裡不抄那個做法（那是一個布林資源，判準藏在
     * `values-*` 的目錄名裡，測不到），改成把真正的問句寫出來：
     *
     *     **扣掉鍵盤之後，宿主還剩不剩得下一個輸入框？**
     *
     * 那才是 extract 模式存在的理由。兩台裝置的實測值：
     *
     *   手機橫屏  411 dp − 324 dp = **87 dp**  → 排不下（宿主的 189 px 全被
     *                                            它自己的標題列佔滿，
     *                                            `uiautomator dump` 裡一個
     *                                            輸入框節點都沒有）→ 要全螢幕
     *   平板橫屏 1066 dp − 337 dp = **729 dp** → 綽綽有餘 → 不要全螢幕
     *
     * [MIN_HOST_HEIGHT_DP] 取 200 dp，落在 87 與 729 之間，而且它有意義：
     * 一條 Material 的 app bar（56 dp）＋ 一個輸入框（約 48 dp）＋ 上下留白，
     * 再加一點看得到自己在什麼脈絡裡的內容。往上或往下挪幾十 dp 都不會改變
     * 這兩台的結論 —— 那正是門檻該有的樣子。
     *
     * ⚠ **鍵盤高度是參數,不是常數。** 使用者拖曳調過高度、主題換過、
     *   佈局的 §9.2 覆寫，都會讓它變。呼叫端用既有的
     *   `KeyGeometry.budget()` 算,所以這個判斷跟著使用者的設定走。
     *
     * @param landscape 顯示器目前是不是橫向（`Configuration.ORIENTATION_LANDSCAPE`）
     * @param screenHeightDp 目前這個方向上的螢幕高（`Configuration.screenHeightDp`）
     * @param imeHeightDp 我們的視窗會佔掉多少高（鍵盤預算 ＋ 候選列）
     */
    fun useFullscreen(
        imeOptions: Int,
        landscape: Boolean,
        screenHeightDp: Float,
        imeHeightDp: Float,
    ): Boolean {
        if (!landscape) return false
        if (hostForbidsFullscreen(imeOptions)) return false
        return hostCannotFitEditor(screenHeightDp, imeHeightDp)
    }

    /**
     * 扣掉我們的視窗之後，宿主還剩不剩得下一個輸入框。
     *
     * 抽出來是為了測得到，也為了讓「量的是什麼」寫在名字上:
     * 這不是「螢幕夠不夠大」,是「**宿主還看不看得見自己**」。
     *
     * ⚠ 兩個參數任一 ≤ 0 時回 `true`(＝照舊進全螢幕)。那是「還不知道」的
     *   情形(主題還沒載入、`Configuration` 還沒到手),而在還不知道的時候,
     *   走那條**看得見自己打什麼**的路是比較安全的一邊 —— 反過來猜錯的代價
     *   是使用者完全看不到自己在打什麼。
     */
    fun hostCannotFitEditor(screenHeightDp: Float, imeHeightDp: Float): Boolean {
        if (screenHeightDp <= 0f || imeHeightDp <= 0f) return true
        return screenHeightDp - imeHeightDp < MIN_HOST_HEIGHT_DP
    }

    /**
     * 宿主至少要剩這麼高，才算「還看得見自己在打什麼」。
     *
     * 一條 app bar（56 dp）＋ 一個輸入框（約 48 dp）＋ 留白與一點脈絡。
     * 實測的兩個端點是 87 dp（手機橫屏，排不下）與 729 dp（平板橫屏，
     * 綽綽有餘），這個門檻落在中間，而且離兩端都很遠。
     */
    const val MIN_HOST_HEIGHT_DP = 200f

    /** 宿主明說不要全螢幕（`EditorInfo.IME_FLAG_NO_FULLSCREEN`）。 */
    fun hostForbidsFullscreen(imeOptions: Int): Boolean =
        (imeOptions and EditorInfo.IME_FLAG_NO_FULLSCREEN) != 0

    /* ───────────────── 三、這顆換行鍵按下去會「送出」嗎 ───────────────── */

    /**
     * 在這個編輯框裡,換行鍵是**換一行**,還是**把東西交出去**。
     *
     * ── 為什麼要問這個問題 ──────────────────────────────────────────────
     * 覆核實測(部署中、`dev.rime.imetest` 的多行框):
     *
     *     HOST-after-specials=[\n]
     *
     * 換行真的寫進了宿主。在 Messages 那是換一行,收得回來;而**同一顆鍵在
     * LINE / WhatsApp / Telegram 開了 enter-to-send 的人身上,是把半句話送給
     * 另一個人** —— 那一下收不回來。兩者在我們這一側完全一樣(都是
     * `AndroidKeyMap.RETURN`),差別只在宿主的 [EditorInfo] 上。
     *
     * ── 判準就是 AOSP 自己的判準 ────────────────────────────────────────
     * `fallbackKey()` 送換行的第一步是 `sendDefaultEditorAction(true)`,
     * 而 `InputMethodService` 那一支的條件恰好是:
     *
     *   · 宿主**沒有**掛 `IME_FLAG_NO_ENTER_ACTION`,而且
     *   · `imeOptions` 的 action 欄位不是 `IME_ACTION_NONE`
     *
     * 成立就 `performEditorAction(...)`(＝送出／前往／搜尋),不成立才回落成
     * 一個真的 `KEYCODE_ENTER`(＝換一行)。所以這支函式抄的是**同一個條件**:
     * 它回 true 的那一刻,正是 `fallbackKey()` 會走 performEditorAction 的那一刻。
     * 判準與行為分家的話,畫面上寫的與實際發生的就會對不上 ——
     * 而那正是這一輪要修的東西。
     *
     * ⚠ 多行輸入框(`TYPE_TEXT_FLAG_MULTI_LINE`)不必在這裡另外判:`TextView`
     *   自己會在那種框上補 `IME_FLAG_NO_ENTER_ACTION`,所以它們一律回 false。
     *   實測見上面那個 `[\n]`。
     */
    fun enterCommitsToHost(imeOptions: Int): Boolean {
        if ((imeOptions and EditorInfo.IME_FLAG_NO_ENTER_ACTION) != 0) return false
        return (imeOptions and EditorInfo.IME_MASK_ACTION) != EditorInfo.IME_ACTION_NONE
    }

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
     * ⚠ **使用者現在按得到的觸發路徑只有「用手指點宿主的輸入框」。**
     *   佈局裡那些 `swipe: { left: { tap: "cursor:left" } }` 現在**觸達不到**
     *   （Android 端沒有實作 swipe 分派，見 §9.6「swipe 是 OPTIONAL」與
     *   `LayoutSwipeReachabilityTest`）。這一條是用實體方向鍵與觸控點擊
     *   驗過的，不是靠那條還沒接上的路。
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
