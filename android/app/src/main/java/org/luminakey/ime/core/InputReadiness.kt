package org.luminakey.ime.core

/**
 * 「引擎還沒好」的那十幾秒,這一顆鍵該做什麼。
 *
 * ── 為什麼非有這個判斷不可(工單 #105 的實測)────────────────────────────
 * 全新安裝、啟用之後到別的 app 點輸入框,鍵盤畫得出來、按得動,而 librime
 * 還在編詞庫。實測(emulator-5558,release APK `27555ec`,`pm clear` 之後
 * 走完整條首次部署):
 *
 *   05:11:44.509  phase → DEPLOYING
 *   打 n i h a o
 *   宿主輸入框 = **`nihao`**            ← 五個拉丁字母直接進了真實訊息
 *   05:11:57.283  phase → READY        (12 774 ms)
 *
 * 因果鏈只有三步,而且每一步單獨看都很合理:
 *   1. 還沒 READY → `session == INVALID_SESSION`;
 *   2. `RimeCore.processKey()` 因此回 false;
 *   3. 呼叫端把 false 一律讀成「引擎不要這顆鍵」→ `fallbackKey()` →
 *      `commitText("n")`。
 *
 * **第 3 步把兩件完全不同的事合成同一個結果**:
 *   · 引擎在,但它說這顆鍵不歸它管(英數模式的標點、功能鍵)→ 字面上屏是對的;
 *   · 引擎根本不在 → 字面上屏是**把錯的東西送進使用者的訊息**。
 *
 * 分辨這兩件事不能靠 `processKey` 的回傳值(它們回的是同一個 false),只能靠
 * 「引擎在不在」這個獨立的事實。這支函式就是那個分辨點。
 *
 * ── 為什麼不是「全部擋掉」────────────────────────────────────────────────
 * 退格與換行的輸出**不取決於引擎**:退格就是退格,換行就是換行,使用者按下去
 * 得到的正是鍵面上寫的那件事。把它們一起擋掉,使用者連「把剛才誤打的字刪掉」
 * 「把已經寫好的訊息送出去」都做不到 —— 那是為了修一個問題製造第二個。
 *
 * 空白鍵**不在**這張放行清單上,而這是刻意的:在中文輸入法裡空白的預設語義是
 * 「接受候選」(`SpaceBehavior.ACCEPTS_CANDIDATE`),也就是**引擎改得動它的意思**。
 * 判準因此是一句話:**引擎有可能改變這顆鍵的意思,引擎不在的時候就不要收它**。
 *
 * ── 這一層不負責讓使用者知道 ────────────────────────────────────────────
 * 「按了沒反應」本身是這個專案抓過六次的那種缺陷。所以擋鍵**必須**配一個
 * 看得見的理由,那件事在 `KeyboardView` 那一側(蓋在鍵區上的那一層 +
 * 候選列的高對比提示),不在這裡。這裡只回答「這顆鍵該不該送出去」。
 */
object InputReadiness {

    enum class Decision {
        /** 交給 librime。 */
        ENGINE,

        /** 繞過 librime,字面上屏／直接編輯宿主。 */
        LITERAL,

        /** **什麼都不做。** 引擎不在,而這顆鍵的意思由引擎決定。 */
        HOLD,
    }

    /**
     * @param engineReady `RimeRuntime.isReady` —— 有沒有一個能用的引擎。
     * @param bypassRime 這個編輯框(密碼、數字…)本來就不走引擎,見 `shouldBypassRime`。
     * @param keysym X11 keysym。
     */
    fun decide(engineReady: Boolean, bypassRime: Boolean, keysym: Int): Decision = when {
        // 密碼框那條路從一開始就不要引擎,字面上屏本來就是正確答案。
        bypassRime -> Decision.LITERAL
        engineReady -> Decision.ENGINE
        editsHostDirectly(keysym) -> Decision.LITERAL
        else -> Decision.HOLD
    }

    /**
     * 這顆鍵的輸出與引擎無關嗎。
     *
     * ⚠ 新增成員之前先問:「這個方案有沒有可能讓這顆鍵做別的事?」
     * 空白會(接受候選)、數字會(選字,見工單 #99)、標點會(全形半形),
     * 所以它們都不在這裡。
     */
    fun editsHostDirectly(keysym: Int): Boolean =
        keysym == AndroidKeyMap.BACKSPACE || keysym == AndroidKeyMap.RETURN

    /**
     * 換行鍵的**第二個問句**:送出去之後收得回來嗎。
     *
     * ── 這不是上面那條判準的修正,是另一條軸 ────────────────────────────
     * [decide] 問的是「引擎改不改得動這顆鍵的意思」。換行的答案是**不會**,
     * 所以它是 [Decision.LITERAL],那一條沒有改,也不該改:擋掉換行會讓人連
     * 「把已經寫好的訊息送出去」都做不到,擋掉退格會讓人連刪錯字都做不到。
     *
     * 這一支問的是完全不同的一件事:**這一下按出去,使用者救不救得回來。**
     *   · 多行框裡的換行 → 換一行,退格就刪得掉 → 照送。
     *   · 掛了 editor action 的框(LINE / WhatsApp / Telegram 的 enter-to-send、
     *     搜尋框的「前往」)→ 東西已經交給另一個人／另一個系統 → **收不回來**。
     *
     * 而這一段時間正是使用者最可能亂按的時候:遮罩上寫著「打不出字」,他會
     * 按一下看看鍵盤活了沒有。那一下如果落在換行上,就是一則沒寫完的訊息。
     *
     * ── 代價,以及為什麼仍然值得 ────────────────────────────────────────
     * 代價是真的:**訊息已經寫好、只差按下送出的人,要多等那幾秒。**
     * 但兩邊的失敗不對稱 ——
     *   · 擋錯了:等幾秒,訊息一個字都沒少,遮罩上寫著還要等多久;
     *   · 放錯了:半句話已經在對方的手機上,而我們連道歉的位置都沒有。
     * 可回復的那一邊才是不確定時該站的地方。
     *
     * ⚠ 擋下來**一定要看得見**,而且要看得見的是「這一下被擋了」,不是一句
     *   本來就在畫面上的話。呼叫端負責在遮罩上換一句話(`heldKeyNotice`)。
     *
     * @param enterCommitsToHost 見 [HostEditorPolicy.enterCommitsToHost]。
     */
    fun holdsEnter(
        engineReady: Boolean,
        bypassRime: Boolean,
        keysym: Int,
        enterCommitsToHost: Boolean,
    ): Boolean {
        if (engineReady || bypassRime) return false
        if (keysym != AndroidKeyMap.RETURN) return false
        return enterCommitsToHost
    }
}
