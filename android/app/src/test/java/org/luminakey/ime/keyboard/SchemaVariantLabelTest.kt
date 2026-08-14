package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * 工單 #107:同一個畫面同時寫著「立好」與「臺灣正體」。
 *
 * ① 判準本身;② `faceOf()` 真的接上了它(空白鍵鍵面走的就是那一支)。
 */
class SchemaVariantLabelTest {

    /* ─────────────── ① 判準 ─────────────── */

    /** 走查實際看到的那一格:引擎在吐簡體,空白鍵卻寫著臺灣正體。 */
    @Test
    fun `矛盾時換成現在生效的那一個`() {
        assertEquals(
            "朙月拼音·简体",
            SchemaVariantLabel.display("朙月拼音·臺灣正體", simplified = true),
        )
        assertEquals(
            "注音·繁體",
            SchemaVariantLabel.display("注音·简化字", simplified = false),
        )
    }

    /**
     * 一致的時候**不准動**。`臺灣正體` 比 `繁體` 精確,換掉它是把資訊變粗。
     */
    @Test
    fun `一致時保留方案自己的說法`() {
        assertEquals(
            "朙月拼音·臺灣正體",
            SchemaVariantLabel.display("朙月拼音·臺灣正體", simplified = false),
        )
        assertEquals(
            "朙月拼音·简化字",
            SchemaVariantLabel.display("朙月拼音·简化字", simplified = true),
        )
    }

    /** 使用者沒有覆寫過(`UserPrefs.simplification == null`)→ 讓方案自己說。 */
    @Test
    fun `沒有人覆寫過就原樣不動`() {
        assertEquals(
            "朙月拼音·臺灣正體",
            SchemaVariantLabel.display("朙月拼音·臺灣正體", simplified = null),
        )
    }

    /**
     * 方案名沒有宣告字集 → 原樣不動。
     *
     * 這一條擋的是最容易犯的那個錯:對每一個方案名都接上「繁體」,於是
     * 日文、韓文、英文方案的空白鍵上會多出兩個沒有意義的漢字。
     */
    @Test
    fun `沒有字集宣告的方案名一個字都不改`() {
        for (name in listOf("朙月拼音", "倉頡五代", "Emoji", "日本語", "五筆·簡入繁出", "")) {
            for (v in listOf(true, false, null)) {
                assertEquals(
                    "「$name」不該因為簡繁開關而改變",
                    name,
                    SchemaVariantLabel.display(name, v),
                )
            }
        }
    }

    @Test
    fun `認得出宣告，也認得出不是宣告`() {
        assertEquals(false, SchemaVariantLabel.claimOf("朙月拼音·臺灣正體"))
        assertEquals(true, SchemaVariantLabel.claimOf("朙月拼音·简化字"))
        assertNull(SchemaVariantLabel.claimOf("朙月拼音"))
        assertNull("「簡入繁出」不是字集宣告", SchemaVariantLabel.claimOf("五筆·簡入繁出"))
    }

    /* ─────────────── ② 接線 ─────────────── */

    /**
     * `faceOf()` 的 `SCHEMA_NAME` 那一行真的問過判準。
     *
     * 驗的是**那一行**而不是整檔 grep:`SchemaVariantLabel` 出現在 import 裡
     * 也算「檔案裡有這個字」,而那證明不了任何事。
     */
    @Test
    fun `空白鍵鍵面接上了判準`() {
        val src = File("src/main/java/org/luminakey/ime/keyboard/KeyboardView.kt").readText()
        // 那一條 when 分支的**範圍**:從 `LabelSource.SCHEMA_NAME ->` 到下一個
        // `LabelSource.`。限定範圍而不是整檔 grep —— import 那一行也含
        // `SchemaVariantLabel`,整檔掃會被它餵飽(這個專案抓過四次的形狀)。
        val at = src.indexOf("LabelSource.SCHEMA_NAME ->")
        assertTrue("KeyboardView 裡找不到 SCHEMA_NAME 那一段 —— 對不上實作", at >= 0)
        val next = src.indexOf("LabelSource.", at + 26)
        val arm = src.substring(at, if (next > at) next else src.length)
            .lineSequence().joinToString("\n") { line ->
                // 註解要挖掉:它解釋的正是這幾個識別字。
                val i = line.indexOf("//")
                if (i >= 0 && line.take(i).isBlank()) "" else line
            }
        assertTrue(
            "空白鍵鍵面沒有問過 SchemaVariantLabel,矛盾會原封不動地留在畫面上:\n$arm",
            arm.contains("SchemaVariantLabel.display("),
        )
        assertTrue(
            "沒有把引擎的當下字集餵進去,那就只是換個地方寫死:\n$arm",
            arm.contains("status.isSimplified"),
        )
    }

    /** 反向:上一版那一行(直接回傳方案名)不含判準,順序斷言才有意義。 */
    @Test
    fun `上一版那一行確實沒有問過判準`() {
        val old = "LabelSource.SCHEMA_NAME -> status.schemaName.ifEmpty { null }"
        assertTrue(old.contains("SchemaVariantLabel.display(").not())
    }
}
