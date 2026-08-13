package org.luminakey.ime.keyboard

import android.content.res.AssetManager
import android.util.Log

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
     * TSV：`layout <TAB> schema <TAB> verdict <TAB> measured_on <TAB> note`。
     * `#` 開頭與空行是註解。
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
