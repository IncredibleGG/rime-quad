package org.luminakey.ime.theme

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * `item.highlight_style`：**預設值**與**作用域**。
 *
 * 兩件事各自都被回報過：
 *
 *  1. 上一版把預設從實心塊改成 `underline`。使用者要的是**候選數量**，而
 *     實心塊的寬度成本實測是 **0**（把 `highlight_background` 換成
 *     transparent 之後每一段墨跡座標逐 px 相同）—— 它不是密度的成因。
 *     那是一個沒有人要求、也買不到密度的外觀變更。
 *  2. 這個欄位一度住在**共用**的 `ITEM_KEYS` 裡，於是
 *     `candidates.item.highlight_style` 在 Android 上被靜靜接受、
 *     在 macOS 上是一則 `unknown_field` —— §10 第 9 條要求的四端診斷序列
 *     當場對不上，而那是最難查的一種紅。
 */
class CandidateHighlightScopeTest {

    private fun loadInline(id: String, vararg docs: Pair<String, String>): LoadResult<Theme> =
        ThemeLoader.load(id, MapDocumentSource(docs.toMap()), Platform.ANDROID)

    private val MINIMAL = "format: rime-theme/1\nid: t\n"

    @Test
    fun `預設的高亮畫法是實心塊`() {
        val t = loadInline("t", "t" to MINIMAL).value!!
        assertEquals(
            "預設又被改掉了 —— 那是一個沒有人要求、也買不到密度的外觀變更",
            HighlightStyle.FILL, t.candidates.shared.item.highlightStyle,
        )
        assertEquals(HighlightStyle.FILL, t.candidates.bar.style.item.highlightStyle)
        assertEquals(HighlightStyle.FILL, t.candidates.window.style.item.highlightStyle)
    }

    @Test
    fun `隨附主題沒有一份自己改掉高亮畫法`() {
        for (id in RepoFixtures.themeIds) {
            val t = ThemeLoader.load(id, RepoFixtures.themes, Platform.ANDROID).value!!
            assertEquals(
                "$id 的候選列高亮不是實心塊",
                HighlightStyle.FILL, t.candidates.bar.style.item.highlightStyle,
            )
        }
    }

    /** `candidates.bar.item.highlight_style` 是行動端專屬欄位，寫在這裡不該有診斷。 */
    @Test
    fun `bar 底下認得 highlight_style`() {
        val doc = MINIMAL + "candidates:\n  bar:\n    item:\n      highlight_style: underline\n"
        val r = loadInline("t", "t" to doc)
        assertEquals(
            "candidates.bar.item.highlight_style 竟然有診斷：${RepoFixtures.describe(r.diagnostics)}",
            0, r.diagnostics.size,
        )
        assertEquals(HighlightStyle.UNDERLINE, r.value!!.candidates.bar.style.item.highlightStyle)
    }

    /**
     * ⛔ **共用的 `candidates.item` 底下不認得它** —— 與 macOS 逐字相同。
     *
     * §10 第 9 條的作用域表：`candidates.bar` 之下的欄位只有 Android/iOS 需要
     * 認得，`candidates.item`（共用那一層）則是四端全部。所以要嘛四端都認、
     * 要嘛四端都不認 —— 這裡選後者，因為桌面端的候選窗沒有「六個並排時大色塊
     * 會蓋掉其餘五個」這個問題，欄位本來就該住在 `bar` 底下。
     *
     * 這是唯一不必等別人就成立的解：Windows 今天連主題解析器都沒有（工單 #47）。
     */
    @Test
    fun `共用的 item 底下不認得 highlight_style`() {
        val doc = MINIMAL + "candidates:\n  item:\n    highlight_style: underline\n"
        val r = loadInline("t", "t" to doc)
        val unknown = r.diagnostics.filter { it.code == DiagnosticCode.UNKNOWN_FIELD }
        assertTrue(
            "candidates.item.highlight_style 沒有產生 unknown_field —— " +
                "Android 靜靜接受而 macOS 報錯,四端診斷序列對不上:" +
                RepoFixtures.describe(r.diagnostics),
            unknown.any { it.path.endsWith("highlight_style") },
        )
        assertEquals(
            "不認得的欄位竟然還生效了",
            HighlightStyle.FILL, r.value!!.candidates.shared.item.highlightStyle,
        )
    }

    /** `candidates.window.item` 也不認得（桌面端那一半，作用域同上）。 */
    @Test
    fun `桌面端候選窗底下不認得 highlight_style`() {
        val doc = MINIMAL + "candidates:\n  window:\n    item:\n      highlight_style: outline\n"
        val r = loadInline("t", "t" to doc)
        assertTrue(
            "candidates.window.item.highlight_style 沒有產生 unknown_field:" +
                RepoFixtures.describe(r.diagnostics),
            r.diagnostics.any {
                it.code == DiagnosticCode.UNKNOWN_FIELD && it.path.endsWith("highlight_style")
            },
        )
    }
}
