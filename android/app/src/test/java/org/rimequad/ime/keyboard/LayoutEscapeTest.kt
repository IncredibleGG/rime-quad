package org.rimequad.ime.keyboard

import org.junit.Assert.assertTrue
import org.junit.Test
import org.rimequad.ime.theme.LayoutKind
import org.rimequad.ime.theme.RepoFixtures

/**
 * 「進得去、出不來」的通用防線。
 *
 * 真機回報的缺陷（九宮格按 ABC 之後回不去）不是某一顆鍵寫錯，而是一整類問題：
 * 只要有一份佈局的某個層，**看得見的鍵**裡沒有任何一條路徑走得回起點，
 * 使用者就被鎖死了。逐條列舉「九宮格→英數→九宮格」這種路徑永遠會漏掉下一個，
 * 所以改成把導覽鍵當成一張圖來走。
 *
 * ⚠ **走訪的邏輯本身已經搬到 main 的 [LayoutEscape]。** 使用者自訂鍵位
 * （[applyKeyRemap]）必須通過同一條檢查，而「測試裡一份、驗證裡一份」的兩份
 * 實作一定會漂移 —— 漂移的那一天，被鎖死的是使用者不是我們。本檔因此只剩
 * 夾具與斷言。
 */
class LayoutEscapeTest {

    /**
     * 出發點是**使用者真的可能停在上面的鍵盤**，不含符號／數字面板。
     *
     * 面板不是一種鍵盤，是從某份鍵盤按 `?123` / `!@#` 進去、按「返回」
     * （`switch_layout:@previous`）回來的附屬層；使用者選不到它當常駐鍵盤
     * （[KeyboardTypes] 把 `isAccessory` 濾掉了）。從面板**冷啟動**是一個
     * 不存在的狀態：`@previous` 沒有歷史，只能退到 primary，而 primary 上
     * 當然沒有一顆鍵回得去那個面板 —— 那不是死路，是不存在的場景。
     *
     * 面板本身仍然被完整走過：從 `qwerty` / `cn-t9-pinyin` / `cn-stroke`
     * 走進去再走出來，正是這條測試要守的事。
     */
    @Test
    fun everyReachableLayerCanGetBackToWhereItStarted() {
        val repo = FixtureRepo()
        val starts = RepoFixtures.layoutIds.filter { id ->
            val kind = repo.loadLayout(id).value?.kind
            kind != LayoutKind.SYMBOL && kind != LayoutKind.NUMERIC
        }
        assertTrue("起點不該是空的", starts.size >= RepoFixtures.layoutIds.size - 2)
        for (start in starts) {
            val problems = LayoutEscape.check(start) { LayoutHost(FixtureRepo()) }
            assertTrue(problems.joinToString("\n"), problems.isEmpty())
        }
    }

    /** `layer:<id>` 指向本佈局不存在的層 = 那顆鍵是啞的，也是卡死的來源之一。 */
    @Test
    fun everyLayerReferenceResolvesInsideItsOwnLayout() {
        val repo = FixtureRepo()
        for (id in RepoFixtures.layoutIds) {
            val layout = repo.loadLayout(id).value!!
            val problems = LayoutEscape.checkLayerReferences(layout, RepoFixtures.layoutIds)
            assertTrue(problems.joinToString("\n"), problems.isEmpty())
        }
    }
}
