package org.luminakey.ime.keyboard

/**
 * 組字串**畫給誰看**的那一層 —— 與 librime 的 preedit 刻意分開。
 *
 * ── 使用者的原話 ────────────────────────────────────────────────────────
 * 「但是你顯示 PGM 就很奇怪？」
 *
 * 他是對的。`PGM` 是我們的**內部編碼**（九宮格雙編碼方案的大寫那一半），
 * 不是他的語言。而 [org.luminakey.ime.RimeInputMethodService] 一直把
 * librime 的 preedit **原樣**送進宿主 app 的輸入框。
 *
 * ── 根因：preedit 照定義就是「你按了什麼」 ──────────────────────────────
 * librime `ScriptSyllabifier::GetPreeditString()` 直接抄 `input_` 的子字串，
 * 只在音節邊界插入 `delimiter`，**它不會**把模糊碼回推成規範拼音
 * （回推的是候選的 `comment`，走 `ScriptTranslator::Spell()`）。
 * 詳見 `docs/decisions/t9-model.md` §1.2。
 *
 * 所以這不是設定問題：沒有任何 `preedit_format` 救得了它，因為 preedit
 * 手上根本沒有那個資訊。要讓宿主輸入框看到別的東西，只能在**送進去之前**
 * 轉換一次 —— 那就是這個檔案。
 *
 * ── 這個檔案做什麼、不做什麼 ────────────────────────────────────────────
 * **做**：把「宿主輸入框看到的組字文字」與「librime 的 preedit」拆成兩件事，
 * 並且讓這個轉換是一個**純函式**，測得到（[PreeditDisplayTest]）。
 *
 * **不做**：決定九宮格最終該顯示什麼。那要等 `docs/decisions/t9-model.md`
 * §1.5 的 S1-2（改用高亮候選的 comment，`MG GAM` → `ni hao`）被採納。
 * 這一版先把**沒有意義的東西拿掉**，並且把界線留在 [PreeditParts.Split] 上，
 * 換呈現的時候動的是 [HostPreedit.forHost] 一個函式，切分不用重做。
 */

/* ═══════════════ 切分：讀得懂的 vs 一整組字母的代號 ═══════════════ */

/**
 * 把 preedit 切成「使用者讀得懂的那一段」與「按鍵代碼那一段」。
 *
 * **兩個顯示端共用這一個切分** —— 候選列左端那一格（[InlinePreedit]）與
 * 宿主 app 的輸入框（[HostPreedit]）。各寫一次的話，同一串 preedit 在
 * 鍵盤上與輸入框裡會長得不一樣，而那是使用者最沒辦法理解的一種畫面。
 * （上一輪就是只修了候選列那一格，輸入框那一份原封不動。）
 *
 * ── 判準：**不是**「這個方案是不是九宮格」 ──────────────────────────────
 * 寫死方案 id（`t9_pinyin`）是最糟的做法：市集裡任何第三方九宮格方案都不
 * 受惠，而它壞掉的樣子是「畫面一切正常，只是又印回代碼」——
 * 沒有任何東西會叫。綁死 `ADGJMPTW` 那八個字母一樣：方案換一組代表字母，
 * 這裡就靜靜地失效。
 *
 * 判準是一句能一直成立的話：**送出這個字元的那顆鍵，鍵面上是一整組字母
 * 而不是它自己。**
 *
 *   * 九宮格送 `M` 的鍵印的是 `mno` → `M` 是那一組的代號，對使用者沒有意義。
 *   * 全拼送 `n` 的鍵印的就是 `n`、注音送 `ㄋ` 的鍵印的就是 `ㄋ`
 *     → 「你按了什麼」對他們**就是**有意義的，一個字元都不動。
 *
 * 實作在 [InlinePreedit.groupCodeChars]（讀佈局檔，與方案 id 無關）；
 * 這裡只吃它的結果。
 */
object PreeditParts {

    /**
     * @param raw librime 給的原文，一個字元都沒動。
     * @param settled 已經定下來、使用者讀得懂的那一段：`ni`、已選的漢字、
     *   注音符號、聲調、標點。頭尾的分隔符已收乾淨。
     * @param pending 還沒定下來的按鍵代碼，**保留原樣**（`GAM`）。
     *
     * ── 為什麼兩段要分開存，而不是只留一個字串 ────────────────────────
     * 「已確定的音節」與「還在模糊的尾巴」是兩種不同的狀態。少了界線，
     * `ni` 與 `ni⋯` 就分不出來 —— 前者是「我打完了，就是 ni」，後者是
     * 「ni 已經定了，後面還有沒定的」。使用者看到前者會以為剩下的按鍵不見了。
     *
     * 而且**下一版要換的正是 [pending] 那一段的呈現**：t9-model.md §1.5 的
     * S1-2 要把它換成引擎自己對那一段的讀音（高亮候選的 comment，
     * `MG GAM` → `ni hao`）。界線留在這裡，換呈現就不用重做切分。
     */
    data class Split(
        val raw: String,
        val settled: String,
        val pending: String,
    ) {
        /** 有沒有還沒定下來的按鍵代碼。 */
        val hasPending: Boolean get() = pending.isNotEmpty()
    }

    /**
     * @param groupCodes [InlinePreedit.groupCodeChars] 的結果。
     *   **空集合 = 什麼都不知道**（佈局還沒載進來、或這份佈局一鍵一字母），
     *   那時候整串都算「讀得懂」——「沒有資訊」不可以被當成「確定沒有意義」，
     *   否則鍵盤剛彈出來那幾十毫秒組字串會整個閃掉。
     */
    fun of(preedit: String, groupCodes: Set<Char>): Split {
        if (preedit.isEmpty() || groupCodes.isEmpty()) {
            return Split(raw = preedit, settled = preedit, pending = "")
        }
        val keep = StringBuilder(preedit.length)
        val codes = StringBuilder()
        for (ch in preedit) {
            if (ch in groupCodes) codes.append(ch) else keep.append(ch)
        }
        if (codes.isEmpty()) return Split(raw = preedit, settled = preedit, pending = "")
        // 代碼被抽走之後，speller 插進去的分隔符會懸在頭尾（`ni ` / `'`），
        // 中間也可能連成一串空白。那些分隔符是**代碼的標點**，代碼沒了就
        // 不該留在畫面上。
        val settled = keep.toString()
            .trim { it.isWhitespace() || it == '\'' }
            .replace(SEPARATOR_RUN, " ")
        return Split(raw = preedit, settled = settled, pending = codes.toString())
    }

    private val SEPARATOR_RUN = Regex("\\s{2,}")
}

/* ═══════════════ 宿主 app 輸入框看到的組字文字 ═══════════════ */

/**
 * `InputConnection.setComposingText()` 要送什麼進去。
 *
 * ── 為什麼是「換掉」而不是「整個不送」 ──────────────────────────────────
 * 語燕的做法是宿主輸入框**全程留空**，組字只畫在鍵盤上（t9-model.md §1.3
 * 實測）。那最乾淨，但影響面比看起來大：Android 的無障礙服務、部分輸入框的
 * 「輸入中」樣式、以及宿主 app 自己的自動完成都靠 composing region 存在。
 * 所以這裡採 t9-model.md §1.5 的建議 (B)：**照樣佔著組字區，只是內容換掉**。
 *
 * 於是有一條硬性質：**preedit 非空時，送進去的文字也一定非空**。
 * 空字串會讓組字區當場消失，等於偷偷改成了做法 (A)。
 */
object HostPreedit {

    /**
     * 未定的那一段收成這個記號。與 [InlinePreedit.ELLIPSIS]、
     * [T9Syllables.MORE_LABEL] 同字形，意思都是「還有」。
     */
    const val PENDING_MARK = InlinePreedit.ELLIPSIS

    /**
     * [Shown.cursor] 恆等於這個值。
     *
     * ⚠ 這**不是**字元索引。`setComposingText(text, newCursorPosition)` 的
     * 第二個參數在 Android 的約定裡是：
     *
     *   * `> 0` → 相對於**文字結尾**，`1` 正好是「文字的最後面」；
     *   * `<= 0` → 相對於**文字開頭**。
     *
     * 所以 `1` 與文字長度**無關** —— 這正是這裡要的：轉換過後長度變了
     * （`MG GAM` 6 字 → `⋯` 1 字），游標不必、也**不可以**跟著算。
     */
    const val AFTER_TEXT = 1

    /**
     * @param text 送進宿主輸入框的組字文字。
     * @param cursor `setComposingText()` 的第二個參數，見 [AFTER_TEXT]。
     * @param split 這一串是怎麼切出來的，見 [PreeditParts.Split]。
     */
    data class Shown(
        val text: String,
        val cursor: Int,
        val split: PreeditParts.Split,
    )

    /**
     * 宿主輸入框該看到什麼；**null = 沒有組字，呼叫端要收掉組字區**。
     *
     * ── 目前的政策 ──────────────────────────────────────────────────────
     *   * 沒有按鍵代碼 → **原封不動**送 preedit。全拼的 `nihao`、注音的
     *     `ㄋㄧˇ` 都是使用者剛按過的東西，看得懂，一個字元都不准動。
     *     這條是這個檔案最重要的性質：**這次改動對非九宮格方案是零變更。**
     *   * 有按鍵代碼 → 代碼那一段收成一個 [PENDING_MARK]。
     *     `ni GAM` → `ni⋯`；整串都是代碼（`MG GAM`）→ 只剩 `⋯`。
     *
     * `⋯` 單獨一個看起來很少，但它說的是實話：「正在組字，但還沒有任何
     * 一段定下來」。而 `MG GAM` 說的是謊話 —— 使用者會以為他打出來的是那一串。
     *
     * ── 這個政策要換的時候換哪裡 ──────────────────────────────────────
     * t9-model.md §1.5 的 S1-2：把 [PreeditParts.Split.pending] 換成引擎
     * 對那一段的讀音（高亮候選的 comment，`MG GAM` → `ni hao`）。
     * 那時候要動的只有這個函式與它的 [Shown]，切分與判準都不用碰。
     */
    fun forHost(preedit: String, groupCodes: Set<Char>): Shown? {
        if (preedit.isEmpty()) return null
        val split = PreeditParts.of(preedit, groupCodes)
        val text = if (!split.hasPending) split.raw else split.settled + PENDING_MARK
        return Shown(text = text, cursor = AFTER_TEXT, split = split)
    }
}
