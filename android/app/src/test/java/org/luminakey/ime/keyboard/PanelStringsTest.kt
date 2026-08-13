package org.luminakey.ime.keyboard

import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

/**
 * 鍵盤上那塊面板的在地化防線。
 *
 * ── 為什麼鍵盤這一塊要單獨守 ────────────────────────────────────────────
 * App 那邊有 221 處 `stringResource`,鍵盤這邊一度是 0 —— 整個 `keyboard/`
 * 套件的字全部寫死繁體。原因不難理解:面板是後來才從 App 搬到鍵盤上的,
 * 搬的時候在地化還沒做;而它是**唯一一塊使用者天天看得到的介面**,
 * App 首頁裝完就不會再開了。
 *
 * 漏掉一句的下場和 [org.luminakey.ime.StringCatalogTest] 守的一樣安靜:畫面
 * 不會壞、log 不會叫、build 不會紅,只是一個英文系統的使用者在自己的鍵盤上
 * 看到一句繁體中文。
 *
 * 這裡守兩件 [org.luminakey.ime.StringCatalogTest] 守不到的事。
 */
class PanelStringsTest {

    /**
     * `keyboard/` 底下的硬編漢字**只能變少**。
     *
     * ── 為什麼是棘輪而不是一刀切 ────────────────────────────────────────
     * 這個套件裡的漢字有四種,不能一律當成缺陷:
     *
     *   1. **介面文案** —— 面板上的字。這一輪處理完了,必須是 0。
     *   2. **鍵面** —— 「中」「繁」「简」「全」「半」。§9.4 的鍵面,四端印同一份,
     *      不隨介面語言變:一顆中文輸入法的中／En 鍵在英文系統上仍然印「中」。
     *   3. **代號** —— [KeyboardTypes] 的分組鍵。它們是純函式裡的固定值(要能被
     *      JVM 測試斷言),翻譯在 [localizedGroupTitle] 做。
     *   4. **log 與比對用的關鍵字** —— `Log.i("佈局 → …")`、
     *      `name.contains("香港")`。前者不上畫面,後者翻了反而會壞。
     *
     * 分辨這四種要逐條讀,那是「引擎層約 200 條訊息在地化」那件工作的事,
     * 而它還卡在一個設計裁決上(結構化錯誤型別 vs 讓解析器吃 Context,
     * 見 docs/coordination.md §5)。在那之前,這裡先當**棘輪**:
     * 已經處理完的檔案必須是 0,還沒處理的鎖住現在的數字。
     *
     * 這樣至少保證一件事:在那件工作落地之前,硬編中文**不會再長回來**。
     * 數字往下改是進度,往上改要有理由。
     */
    @Test
    fun `硬編漢字只能變少`() {
        val actual = kotlinSources().associate { it.name to cjkLiterals(it).size }
        val unexpected = actual.keys.filterNot { it in DONE || it in PENDING }
            .filter { (actual[it] ?: 0) > 0 }
        assertTrue(
            "這幾個檔案是新的,先決定它裡面的漢字屬於哪一種(見本測試的檔頭):\n  " +
                unexpected.joinToString("\n  ") { "$it = ${actual[it]}" },
            unexpected.isEmpty(),
        )

        val drifted = mutableListOf<String>()
        for (name in DONE) {
            val n = leftoverIn(name)
            if (n > 0) drifted += "$name 應該是 0,現在有 $n 條寫死的漢字文案"
        }
        for ((name, locked) in PENDING) {
            val n = actual[name] ?: continue
            if (n > locked) {
                drifted += "$name 從 $locked 變成 $n —— 又長回來了"
            } else if (n < locked) {
                drifted += "$name 從 $locked 降到 $n(這是好事)—— 把 PENDING 裡的數字改成 $n"
            }
        }
        assertTrue(drifted.joinToString("\n  ", prefix = "\n  "), drifted.isEmpty())
    }

    /** [DONE] 的檔案扣掉鍵面與預覽樣本之後應該一條不剩。 */
    private fun leftoverIn(name: String): Int {
        val f = kotlinSources().first { it.name == name }
        return cjkLiterals(f).count { (_, lit) ->
            lit !in KEY_FACE_ALLOWED && lit !in SAMPLE_ALLOWED
        }
    }

    /**
     * 面板上的字有**硬性長度上限**,而且超過不會換行,會被默默截掉。
     *
     * 標籤欄固定 64dp、格子是面板寬的三分之一、分段控制一格約 90dp,而畫面上
     * 每一個 Text 都是 `maxLines = 1`。一句英文長到放不下時,使用者看到的是
     * 「Long press」變成「Long pre…」,或者更糟 —— 分段控制沒有設 overflow,
     * 直接切掉,看起來就只是一個比較短的詞,完全不像故障。
     *
     * 上限用字元數估,不是精確的量測(那要 Compose 的 TextMeasurer,JVM 測試
     * 拿不到)。它擋的是「順手把 App 那句 25 個字的說明貼過來」這種等級的錯,
     * 精細的字寬還是得靠真機截圖 —— 見 scripts/verify_rime_compose.sh。
     */
    @Test
    fun `面板字串短到放得進格子裡`() {
        val en = loadStrings("values")
        val tooLong = BUDGET.mapNotNull { (key, limit) ->
            val v = en[key] ?: return@mapNotNull "$key 不存在於 values/strings.xml"
            if (v.length > limit) "$key = \"$v\"(${v.length} > $limit)" else null
        }
        assertTrue(
            "這幾句在面板上會被截掉,而截掉看起來只是「這個詞比較短」:\n  " +
                tooLong.joinToString("\n  "),
            tooLong.isEmpty(),
        )
    }

    /* ────────────────────────────── 夾具 ────────────────────────────── */

    private fun kotlinSources(): List<File> =
        File("src/main/java/org/luminakey/ime/keyboard").listFiles().orEmpty()
            .filter { it.isFile && it.name.endsWith(".kt") }
            .sortedBy { it.name }
            .also { assertTrue("找不到 keyboard 套件的原始碼,這條測試已經失效", it.isNotEmpty()) }

    /**
     * 挑出**不在註解裡**的字串常值中含漢字的那些。
     *
     * 刻意寫得笨:逐行看,遇到 `/*` 進註解、`*/` 出註解,`//` 之後不看。這對
     * 一個「有沒有漢字」的檢查夠用,而且看得懂 —— 用不著為了測試扛一個 Kotlin parser。
     */
    private fun cjkLiterals(f: File): List<Pair<Int, String>> {
        val out = mutableListOf<Pair<Int, String>>()
        var inBlock = false
        f.readLines().forEachIndexed { i, raw ->
            var line = raw
            if (inBlock) {
                val end = line.indexOf("*/")
                if (end < 0) return@forEachIndexed
                line = line.substring(end + 2)
                inBlock = false
            }
            val open = line.indexOf("/*")
            if (open >= 0 && line.indexOf("*/", open) < 0) {
                inBlock = true
                line = line.substring(0, open)
            }
            val slash = line.indexOf("//")
            if (slash >= 0) line = line.substring(0, slash)
            for (m in LITERAL.findAll(line)) {
                val text = m.groupValues[1]
                if (text.any { it.code in 0x4E00..0x9FFF }) out += (i + 1) to text
            }
        }
        return out
    }

    private fun loadStrings(dir: String): Map<String, String> {
        val f = File("src/main/res/$dir/strings.xml")
        assertTrue("找不到 ${f.path}", f.isFile)
        val doc = DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(f)
        val out = LinkedHashMap<String, String>()
        val nodes = doc.getElementsByTagName("string")
        for (i in 0 until nodes.length) {
            val e = nodes.item(i)
            val name = e.attributes?.getNamedItem("name")?.nodeValue ?: continue
            out[name] = e.textContent.orEmpty()
        }
        return out
    }

    private companion object {
        private val LITERAL = Regex("\"([^\"\\\\\n]*)\"")

        /** 已經在地化完的檔案。扣掉鍵面與預覽樣本之後必須是 0。 */
        private val DONE = listOf("KeyboardPanels.kt", "KeyboardView.kt")

        /**
         * 還沒處理的檔案與**現在**的條數。往下改是進度,往上改要有理由。
         *
         * 這幾個數字裡混著上面說的第 3、4 種(代號、log、比對關鍵字),所以
         * 它們的終點不一定是 0 —— 逐條讀完之後,對的做法多半是把那幾條標成
         * 「這不是文案」,而不是硬翻。
         */
        private val PENDING = mapOf(
            "ConfigRepository.kt" to 2,
            "KeyRemap.kt" to 26,
            "KeyboardTypes.kt" to 13,
            "LayoutEscape.kt" to 5,
            // 與 LayoutEscape.kt 同一種:建置期測試印給**開發者**看的診斷,
            // 不上使用者畫面。它要說清楚是哪兩層、差幾 %,那句話是給讀
            // CI 輸出的人看的,翻成英文反而讓現場的人看不懂。
            "LayerGeometry.kt" to 4,
            "LayoutHost.kt" to 12,
            "RemappedLayouts.kt" to 2,
            "SchemaLanguages.kt" to 5,
            // 與 SchemaLanguages.kt 同一種:兩句都是 Log,不上使用者畫面。
            "SelectionDigits.kt" to 2,
        )

        /** §9.4 的鍵面。四端印同一份,不隨介面語言改。 */
        private val KEY_FACE_ALLOWED = setOf("中", "全", "半", "简", "繁")

        /** 候選字預覽用的樣本 —— 要示範的正是中文字看不看得清楚。 */
        private val SAMPLE_ALLOWED =
            setOf("你好", "妳好", "擬好", "泥壕", "你毫", "尼號", "妮豪", "泥號", "擬耗")

        /**
         * 英文預設的字元上限。數字是照畫面上的量體推出來的:
         *   · 24 —— 只有一行、橫跨整個面板的說明(`↓ 直接按…`)
         *   · 16 —— 分段控制的一格(面板寬減 64dp 再除以段數)
         *   ·  12 —— 標籤欄(固定 64dp)與格子的名字(面板寬的三分之一)
         */
        private val BUDGET = mapOf(
            "panel_title_quick" to 12,
            "panel_group_look" to 16,
            "panel_group_output" to 16,
            "panel_height" to 12,
            "panel_appearance" to 12,
            "panel_feel" to 12,
            "panel_keyboard_type" to 12,
            "panel_candidates" to 12,
            "panel_text" to 12,
            "panel_height_standard" to 12,
            "panel_height_taller" to 12,
            "panel_height_shorter" to 12,
            "panel_keyboard_type_auto" to 12,
            "panel_hints" to 12,
            "panel_hints_show" to 16,
            "panel_hints_hide" to 16,
            "panel_long_press" to 12,
            "panel_candidate_count" to 12,
            "panel_candidate_size" to 12,
            "panel_space_commits" to 16,
            "panel_space_always" to 16,
            "panel_variant_traditional_short" to 6,
            "panel_variant_simplified_short" to 6,
            "panel_punct_full_short" to 6,
            "panel_punct_half_short" to 6,
            "panel_height_reset" to 12,
            "panel_height_done" to 12,
            "panel_feel_try" to 34,
            "panel_candidates_preview" to 40,
            "panel_height_drag" to 24,
            "panel_all_settings" to 16,
            // 面板也共用 App 設定頁的幾個 key,它們同樣要塞得進 64dp 的標籤欄。
            "appearance_light_dark" to 16,
            "appearance_colours" to 12,
            "appearance_follow_phone" to 16,
            "appearance_always_light" to 16,
            "appearance_always_dark" to 16,
            "feel_sound" to 12,
            "feel_vibration" to 12,
            "text_characters" to 12,
            "text_punctuation" to 12,
            "text_space_key" to 12,
            "text_traditional" to 16,
            "text_simplified" to 16,
            "text_punct_full" to 16,
            "text_punct_half" to 16,
        )
    }
}
