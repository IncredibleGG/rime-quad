package org.luminakey.ime.store

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * 市集預檢的 WARNING **不可以在成功路徑上被丟掉**。
 *
 * ── 這條測試對應的是什麼 ────────────────────────────────────────────────
 * `StoreController.finish(ok, message, details)` 的成功分支原本只有
 * `showToast(message)` —— `details` 在整條成功路徑上一次都沒被讀，而三個呼叫端
 * 全部是 `ok = true`。SchemaPreflight 自陳市集 98 個方案裡有 20 個會產生
 * WARNING（倉頡／五筆／粵拼／鄭碼那些反查詞典不在的），使用者只看到綠色的
 * 「已啟用並部署完成」，幾天後才發現筆畫反查打不出東西，而且毫無線索。
 *
 * 對照組是 `BackupController`：它兩條路都保留 notes，`BackupSection` 真的畫出來。
 */
class FinishUiTest {

    @Test
    fun `成功但帶著警告要停在對話框上`() {
        val warning = "倉頡的反查詞典不在，筆畫反查會打不出東西"
        val ui = finishUi(ok = true, message = "已啟用並部署完成", details = listOf(warning))

        assertTrue("成功帶警告卻只給了一個幾秒就消失的 snackbar", ui is FinishUi.Dialog)
        ui as FinishUi.Dialog
        assertTrue("這是成功，標題不該說失敗", ui.ok)
        assertEquals(
            "警告被丟掉了 —— 那就是一個使用者永遠不會知道自己少了什麼的功能",
            listOf(warning),
            ui.details,
        )
    }

    @Test
    fun `成功而且沒話要說只給 snackbar`() {
        // 使用者按「重新整理字詞」的目的是把它弄好，不是讀一份報告。
        val ui = finishUi(ok = true, message = "字詞整理完成", details = emptyList())
        assertTrue("沒話要說卻多收一次過路費", ui is FinishUi.Toast)
        assertEquals("字詞整理完成", (ui as FinishUi.Toast).message)
    }

    @Test
    fun `空白的細節不算話`() {
        // 空字串只會在對話框上留一個空的項目符號，那比沒有更糟。
        val ui = finishUi(ok = true, message = "完成", details = listOf("", "   "))
        assertTrue(ui is FinishUi.Toast)
    }

    @Test
    fun `失敗一律停在對話框上`() {
        val ui = finishUi(ok = false, message = "沒有成功", details = listOf("原因"))
        assertTrue(ui is FinishUi.Dialog)
        ui as FinishUi.Dialog
        assertTrue(!ui.ok)
        assertEquals(listOf("原因"), ui.details)
    }

    @Test
    fun `失敗而且沒有細節也要停在對話框上`() {
        // 失敗訊息裡有「請重新啟動 app」這種需要他採取行動的指示，不能自己消失。
        assertTrue(finishUi(ok = false, message = "逾時", details = emptyList()) is FinishUi.Dialog)
    }

    /**
     * 接線：`StoreController` 真的走這支純函式。
     *
     * 純函式全綠、而 controller 裡還留著自己那個 `if (ok) showToast(...)`，
     * 是這個缺陷最可能的復發方式。這是文字比對，不是跑一次 controller ——
     * 它證明得了「決定權在 finishUi 手上」，證明不了對話框長什麼樣。
     */
    @Test
    fun `StoreController 真的用 finishUi 決定要不要停在對話框上`() {
        val f = File("src/main/java/org/luminakey/ime/store/StoreController.kt")
        assertTrue("找不到 ${f.path}", f.isFile)
        val src = f.readText()
        assertTrue("StoreController.kt 只讀到 ${src.length} 個字元，路徑大概錯了", src.length >= 5000)
        assertTrue(
            "StoreController 沒有呼叫 finishUi() —— 成功要不要停在對話框上又變成" +
                "寫在 finish() 裡、沒有人驗得到的一個 if 了",
            src.contains("finishUi("),
        )
    }
}
