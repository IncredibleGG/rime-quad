package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 面板這一側的「進得去、出不來」防線。
 *
 * [LayoutEscapeTest] 守的是佈局的層與層之間；這一份守的是鍵盤上的面板。
 * 兩者是同一條規矩的兩半：**任何面板都不能讓使用者出不去。**
 *
 * ⚠ 這份測試涵蓋的只是「按 ‹ 或系統返回鍵回得去」這條便利回路。真正的保證
 * 是結構性的 —— 每個面板都只蓋住上面幾列，底列全程露出來，所以使用者其實
 * 隨時能直接繼續打字。那一條由版面（PanelFrame 的高度算法）保證，
 * 見 KeyboardView.panelHeightLeavingBottomRow。
 */
class PanelRouteTest {

    /** 走幾步。目前最深的一條是 編輯器 → QUICK → NONE，共 2 步。 */
    private val limit = 4

    @Test
    fun everyPanelGetsBackToNoPanel() {
        for (route in PanelRoute.entries) {
            var cur = route
            var steps = 0
            while (cur != PanelRoute.NONE && steps < limit) {
                val next = cur.back()
                assertTrue(
                    "面板 $route：back() 停在 $cur 不動了 —— 這是死路",
                    next != cur,
                )
                cur = next
                steps++
            }
            assertEquals("面板 $route 在 $limit 步之內回不到 NONE", PanelRoute.NONE, cur)
        }
    }

    @Test
    fun editorsGoBackToTheTiles() {
        // 就地編輯器是從那六格點進去的，`‹` 就該回到六格 —— 使用者剛調完高度，
        // 下一件想做的事多半還在同一個面板上。
        for (route in listOf(
            PanelRoute.HEIGHT,
            PanelRoute.FEEL,
            PanelRoute.CANDIDATES,
            PanelRoute.APPEARANCE,
            PanelRoute.TEXT,
        )) {
            assertEquals(PanelRoute.QUICK, route.back())
        }
    }

    @Test
    fun topLevelPanelsCloseOutright() {
        // 六格與鍵盤類型都是第一層：`‹` 沒有上一層可回，直接收掉整個浮層。
        assertEquals(PanelRoute.NONE, PanelRoute.QUICK.back())
        assertEquals(PanelRoute.NONE, PanelRoute.TYPES.back())
    }

    @Test
    fun stripPanelsDoNotCoverTheKeys() {
        // 手感與候選字必須把鍵盤讓出來：前者要按得到鍵才聽得到聲音、
        // 摸得到震動，後者要看得到真的候選字。
        assertTrue(!PanelRoute.FEEL.coversKeys)
        assertTrue(!PanelRoute.CANDIDATES.coversKeys)
        assertTrue(!PanelRoute.HEIGHT.coversKeys)
        assertTrue(PanelRoute.QUICK.coversKeys)
        assertTrue(PanelRoute.TYPES.coversKeys)
    }
}
