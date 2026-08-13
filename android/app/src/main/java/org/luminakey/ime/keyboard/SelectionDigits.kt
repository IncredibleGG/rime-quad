package org.luminakey.ime.keyboard

import android.content.res.AssetManager
import android.util.Log
import org.luminakey.ime.theme.LayoutKey
import org.luminakey.ime.theme.LayoutLayer
import org.luminakey.ime.theme.SendSpec

/**
 * 「在這一份佈局配這一個方案上，按數字鍵**真的**選得到第 N 個候選嗎」——
 * 一份**由真機量出來**的對照表（§8.6.1.1）。
 *
 * ── 為什麼這件事非得量不可 ──────────────────────────────────────────────
 * 序號 `1 2 3` 只有一個用途：讓使用者按數字鍵選第 N 個。而「鍵送得出 3」與
 * 「按 3 會選第 3 個」之間隔著 librime 的兩層攔截，兩層都**不在佈局檔裡**，
 * 用今天的 `rs_` API 也問不出來（`rime_shell.h` 沒有任何 config 讀取介面，
 * `rs_candidate.label` 又分不出「引擎說按 3」與「門面自己補的序號」——
 * `bopomofo_tw` 給的是 `⇧1 ⇧2 ⇧3`，`luna_pinyin` 給的是門面補的 `1 2 3`，
 * 兩者在 ABI 上長得一模一樣）。實測的三種結局：
 *
 * | 佈局 ＋ 方案 | 按 3 之後 |
 * |---|---|
 * | `cn-qwerty-numrow` ＋ `luna_pinyin_tw` | ✅ 上屏第 3 個 |
 * | `cn-t9-pinyin-numrow` ＋ `t9_pinyin` | ❌ 輸入框變成 `3⋯`，**原本打好的組字被毀掉** |
 * | `bopomofo-dachen` ＋ `bopomofo_tw` | ❌ 那顆鍵是注音字母，數字被 speller 吃掉 |
 *
 * ── ⛔ 這份表不准用手寫 ────────────────────────────────────────────────
 * 手寫的那一刻它就開始腐爛：方案的 `speller/alphabet`、`recognizer/patterns`
 * 一改，或佈局多一排數字，表就過期，而過期的症狀是「畫面上有序號、按下去
 * 毀掉組字」—— 本專案點名過七次的那一類。
 *
 * 所以 `core/selection-digit.tsv` 由 `scripts/verify_selection_digit.sh`
 * **在真機上按數字鍵**量出來（`--bless`），平常那支腳本則拿它當斷言：
 * 量到的與表裡寫的不同就紅，畫面上畫了而表說不行也紅。
 *
 * ── fail-closed ────────────────────────────────────────────────────────
 * 讀不到檔、解析不了、查無此列 —— **一律當成「不畫」**。代價是每格約 9.9 dp
 * 的版面（實測：關掉序號之後密度不變，仍是 6 個），換到的是不會再交付一次
 * 「看得到、按不到」。第三方方案／佈局同理：證不出來就不畫。
 */
object SelectionDigits {

    private const val TAG = "SelectionDigits"

    const val ASSET_PATH = "rime/selection-digit.tsv"

    /** 表裡「這一格可用」的字面。其餘任何值（含 `no`、`unknown`、空）都是不可用。 */
    const val VERDICT_YES = "yes"

    /** key = `"<layout>\t<schema>"`。 */
    @Volatile
    private var usable: Set<String> = emptySet()

    /** 讀進來幾列（含 `no`）—— 只給診斷用，判準不看它。 */
    @Volatile
    var rowCount: Int = 0
        private set

    fun works(layoutId: String?, schemaId: String?): Boolean {
        if (layoutId.isNullOrEmpty() || schemaId.isNullOrEmpty()) return false
        return usable.contains("$layoutId\t$schemaId")
    }

    /**
     * 讀 APK 內那一份。
     *
     * 讀不到只留一條警告：表不在等於「一格都不畫序號」，那是 fail-closed 的
     * 方向 —— 少一段版面提示，不會有任何一顆按不到的東西被畫出來。
     */
    fun loadShipped(assets: AssetManager) {
        val text = runCatching {
            assets.open(ASSET_PATH).use { it.readBytes().toString(Charsets.UTF_8) }
        }.getOrElse {
            Log.w(TAG, "讀不到 $ASSET_PATH —— 一律不畫候選序號（fail-closed）", it)
            return
        }
        val parsed = parse(text)
        usable = parsed.first
        rowCount = parsed.second
        Log.i(TAG, "序號對照表：$rowCount 列，其中 ${parsed.first.size} 格實測可用")
    }

    /** 測試用：直接指定「哪幾格可用」。 */
    fun setForTest(pairs: Collection<Pair<String, String>>) {
        usable = pairs.map { "${it.first}\t${it.second}" }.toSet()
        rowCount = pairs.size
    }

    /**
     * TSV **六欄**：
     * `layout <TAB> schema <TAB> verdict <TAB> compose <TAB> measured_on <TAB> note`。
     * `#` 開頭與空行是註解。
     *
     * ⚠ 這一段從前少寫了 `compose`（那一欄記著量測時打的是哪一串按鍵，
     *   `scripts/verify_selection_digit.sh` 讀它來重現）。本函式只讀前三欄，
     *   所以少寫一欄不會壞掉 —— 但下一個照著 KDoc 產生表的人會少寫一欄，
     *   而那支腳本會在解析時安靜地把 `measured_on` 當成 `compose`。
     *
     * @return （可用的 key 集合, 有效資料列數）
     */
    fun parse(text: String): Pair<Set<String>, Int> {
        val yes = HashSet<String>()
        var rows = 0
        for (raw in text.lineSequence()) {
            val line = raw.trim()
            if (line.isEmpty() || line.startsWith("#")) continue
            val cols = raw.split('\t').map { it.trim() }
            if (cols.size < 3) continue
            val layout = cols[0]
            val schema = cols[1]
            if (layout.isEmpty() || schema.isEmpty()) continue
            rows++
            if (cols[2].lowercase() == VERDICT_YES) yes.add("$layout\t$schema")
        }
        return yes to rows
    }
}

/**
 * ⛔ **「專用數字列」的按鍵,在組字中由 app 層接管 —— 不送進引擎。**
 *
 * ── 問題長什麼樣(工單 #99)────────────────────────────────────────────
 * `cn-t9-pinyin-numrow` 就在鍵盤類型選單第二項。九宮格打 `MG GAM` 之後按
 * 數字列的 `3`:組字沒了,宿主輸入框變成 `3⋯`。
 *
 * 根因不在佈局檔,在 librime 的 recognizer:`core/data/shared/default.yaml` 的
 * `recognizer/patterns.uppercase` 是 `"[A-Z][-_+.'0-9A-Za-z]*$"`,字元集**含
 * `0-9`**;而九宮格的鍵刻意送大寫 `A/D/G/J/M/P/T/W`(`t9_pinyin.schema.yaml`
 * 檔頭寫著「就是為了讓數字鍵完全不進 speller」)。於是整串組字永遠落在那個
 * pattern 裡,數字被 recognizer 收走、附加到輸入串,**走不到選字器**。
 * `rs_process_key` 回 true(consumed),所以連 fallbackKey 那條路都不走。
 *
 * **當初為了避開 speller 吃數字而選大寫,剛好踩進 recognizer 吃數字。**
 *
 * ── 為什麼是 app 層攔截,而不是改方案 ─────────────────────────────────
 * 改 `recognizer/patterns` 是動四端共用的 `core/data/shared/default.yaml`,
 * 而那個 pattern 服務的是「打 `Https://` 這種東西時不要被拆成拼音」——
 * 為了數字列去動它,會在另一個地方長出新的缺陷。而且同樣的形狀還有第二種
 * (`speller/alphabet` 含數字,`bopomofo` 就是),改一個 pattern 修不到它。
 *
 * app 層攔截把「數字鍵在組字中的語義」變成**渲染端的決定**,一次涵蓋所有方案:
 * 它不問引擎、也不必問 —— `rs_select_candidate(頁內相對索引)` 是明確的。
 *
 * ── 判準 ──────────────────────────────────────────────────────────────
 * 一顆鍵是「選字數字鍵」⟺ 三件事同時成立:
 *
 *  1. `label` 就是那個數字(`"3"`),而且
 *  2. `send` 是 `keysym`、名字就是**同一個**數字、不帶 modifier,而且
 *  3. 它所在的層有**整排** `1`–`9`([CandidateDensity.layerHasSelectionDigitRow])。
 *
 * 第 1 條擋掉 `bopomofo-dachen` 的 ㄅ(label 是注音、送 keysym `1`)——
 * 那顆鍵在使用者眼裡是注音字母,不是「第 1 個」。
 * 第 3 條擋掉字母層裡孤零零一顆送數字的鍵。
 *
 * ⚠ **`0` 不算。** `page_size` 是 9,沒有「第 10 個」;而 `d0` / `n0` 送 `0`
 * 就是要打一個零。
 *
 * ⚠ **已知的取捨,寫出來:`cn-t9-pinyin-numrow` 的 `123` 層(`d1`–`d9`)
 * 也滿足這三條**,所以在那一層組字中按數字也會選字。這是**刻意**的:
 * `cn-qwerty-numrow` ＋ `luna_pinyin_tw` 在引擎那一側**本來就是這個行為**
 * (librime 的選字器不知道有「層」這回事),兩者不一致才是缺陷。
 *
 * ⚠ **殘留風險,也寫出來:** 一個 `speller/alphabet` 真的含數字、而且配一份
 * 有專用數字列的佈局的第三方方案,組字中會打不出數字。今天沒有這種組合
 * (`bopomofo` 的鍵不是數字標籤,擋在第 1 條)。`scripts/verify_selection_digit.sh`
 * 量得到它 —— 那一格會變成「按 3 上屏的不是第 3 個」。
 */
object SelectionDigitKeys {

    /** 這一顆是選字數字鍵嗎;是的話回 1..9。呼叫端要先確認整排都在。 */
    fun digitOf(key: LayoutKey): Int? {
        if (key.spacer) return null
        // tap(action)勝過 send(§9.6),有 tap 的鍵不是數字鍵。
        if (key.tap != null) return null
        val send = key.send as? SendSpec.Keysym ?: return null
        if (send.modifiers != 0) return null
        if (send.name.length != 1) return null
        val c = send.name[0]
        if (c !in '1'..'9') return null
        // ⛔ 鍵面必須就是那個數字。`bopomofo-dachen` 的 ㄅ 送 keysym `1`,
        //   但使用者看到的是注音字母 —— 把它當「第 1 個」就是把大千鍵位
        //   當成序號,而那正是上一輪抓到的誤判。
        if (key.label != send.name) return null
        return c - '0'
    }

    /** 整層的判準:整排 1–9 在,才可能有選字數字鍵。 */
    fun rowActive(layer: LayoutLayer?): Boolean =
        CandidateDensity.layerHasSelectionDigitRow(layer)

    /** 按下去要做什麼。 */
    sealed interface Act {
        /** 選第 [indexOnPage] 個(**頁內相對索引**,不得攤平)。 */
        data class Select(val indexOnPage: Int) : Act
        /** 照常送數字(沒有在組字)。 */
        data object SendDigit : Act
        /** 什麼都不做 —— ⛔ **不得毀掉組字**。 */
        data object Ignore : Act
    }

    /**
     * @param digit 1..9。
     * @param composing 現在有組字嗎(preedit 非空)。
     * @param selectableIndices 本頁**畫得出來**的候選,其**頁內索引**。
     *   T9 消歧欄會把一部分候選篩掉 —— 篩掉的那幾個畫面上沒有序號,
     *   按下去選中一個看不見的候選就是新的缺陷。
     */
    fun act(digit: Int, composing: Boolean, selectableIndices: Set<Int>): Act = when {
        !composing -> Act.SendDigit
        (digit - 1) in selectableIndices -> Act.Select(digit - 1)
        // 索引超過本頁候選數(或那一格被篩掉)—— 什麼都不做。
        // ⛔ 這一格從前是「送給引擎」,而引擎把它吃掉並毀了組字。
        else -> Act.Ignore
    }
}
