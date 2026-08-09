package org.luminakey.ime.home

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.R
import org.luminakey.ime.core.RimeRuntime

/**
 * 失敗那一屏**不可以有按不動的按鈕**。
 *
 * ── 這一份守的是什麼 ────────────────────────────────────────────────────
 * 上一版的 `actionOf(stage)` 只看 `SetupStage`，於是五條走到
 * [RimeRuntime.Phase.FAILED] 的路一律得到「重新整理字詞」。實際上那顆按鈕走
 * `StoreController.redeploy()` → `DeployGate.deployAndWait()`，而它第一行是
 * `if (!RimeCore.isInitialized) return NotStarted(…)` ——
 * `.so` 載不起來、ABI 不符、隨附資料解不開、`rs_init` 回 false 這四條路上
 * `nativeInit()` 沒成功過，按幾次都一樣。
 *
 * 使用者按下那一屏唯一的實心按鈕，拿到一句他無能為力的話，然後沒有別的路。
 *
 * ── 為什麼是這幾條斷言，而不是「字串還在檔案裡」───────────────────────
 * 「`grep -q 重新整理字詞`」對整支檔案掃，那個字串在別處出現一次就永遠綠。
 * 這裡驗的是**決策函式的輸出**：哪一種失敗給哪一顆按鈕、配哪一段文案。
 * 把 [actionOf] 的失敗分支改回 `-> NotReadyAction.REFRESH_WORDS`，
 * `按不動的那四種失敗不給按鈕` 立刻紅。
 *
 * ── 它抓不到什麼（誠實說明）────────────────────────────────────────────
 * · 這是純函式層。「畫面上真的照著它畫」由 [NotReadyUiWiringTest] 用文字比對
 *   守（那一份自己也說了它證明不了畫得漂不漂亮）。
 * · 它不會去讀 `DeployGate`，所以「`isInitialized` 的語意哪天變了」不在守備
 *   範圍內 —— 那一條由 `RimeFailureKindTest` 從另一頭（每條失敗路徑都要標種類）
 *   接住。
 */
class FailedAdviceTest {

    /** 全部五種失敗種類（含 NONE）都必須有答案 —— 沒有 else 分支的 when 已保證，這裡再驗一次值。 */
    private val kinds = RimeRuntime.Failure.values()

    /* ─────────────── 1. 按鈕 ─────────────── */

    /**
     * **本檔案存在的理由。**
     *
     * DEPLOY 以外的每一種失敗都不畫「重新整理字詞」，因為它按不動。
     */
    @Test
    fun `按不動的那四種失敗不給按鈕`() {
        val offenders = kinds
            .filter { it != RimeRuntime.Failure.DEPLOY }
            .filter { actionOf(SetupStage.FAILED, it) != NotReadyAction.NONE }
        assertEquals(
            "這幾種失敗仍然畫了按鈕，而它按下去必定得到 NotStarted：$offenders",
            emptyList<RimeRuntime.Failure>(),
            offenders,
        )
    }

    /** 反過來：唯一按得動的那一種**必須**留著按鈕，否則就是把出路也拿掉了。 */
    @Test
    fun `部署失敗仍然給得出重新整理字詞`() {
        assertEquals(
            NotReadyAction.REFRESH_WORDS,
            actionOf(SetupStage.FAILED, RimeRuntime.Failure.DEPLOY),
        )
    }

    /* ─────────────── 2. 文案 ─────────────── */

    /**
     * 沒有按鈕的那幾種，文案**不可以**是「重新整理一次通常就好了」。
     *
     * 這一條抓的是最可能的半套修法：把按鈕拿掉、卻留著原本那句叫人去按按鈕的話。
     * 那比留著按鈕更糟 —— 使用者會去找一顆不存在的按鈕。
     */
    @Test
    fun `不給按鈕的失敗不會叫使用者去按按鈕`() {
        val offenders = kinds
            .filter { actionOf(SetupStage.FAILED, it) == NotReadyAction.NONE }
            .filter { failedBodyRes(it) == R.string.not_ready_failed_body }
        assertEquals(
            "這幾種失敗沒有按鈕，文案卻還是「重新整理一次通常就好了」：$offenders",
            emptyList<RimeRuntime.Failure>(),
            offenders,
        )
    }

    /**
     * 四條死路各自有自己的話。
     *
     * 空間不足與架構不支援是完全不同的處境，共用一句「發生了一些問題」等於
     * 沒說 —— 這正是 Windows 端那條 `三種不同的失敗在畫面上是同一句紅字`。
     */
    @Test
    fun `每一種失敗講的是不同的一件事`() {
        val distinct = listOf(
            RimeRuntime.Failure.DEPLOY,
            RimeRuntime.Failure.LIBRARY_LOAD,
            RimeRuntime.Failure.ABI_MISMATCH,
            RimeRuntime.Failure.UNPACK,
            RimeRuntime.Failure.RIME_INIT,
        ).map { failedBodyRes(it) }
        assertEquals(
            "有兩種失敗指到同一段文案：$distinct",
            distinct.size,
            distinct.toSet().size,
        )
        // 每一個 id 都要是真的資源（aapt 產生的常數不會是 0）。
        assertTrue("有文案指到 0，那是「沒有這個資源」", distinct.all { it != 0 })
    }

    /**
     * 標題也要分。「字詞整理沒成功」對引擎根本沒起來的那四條路是假話，
     * 而使用者會照著標題去找一顆重新整理的按鈕。
     */
    @Test
    fun `引擎沒起來時標題不說字詞整理沒成功`() {
        assertEquals(
            R.string.not_ready_failed,
            failedTitleRes(RimeRuntime.Failure.DEPLOY),
        )
        for (f in kinds) {
            if (f == RimeRuntime.Failure.DEPLOY) continue
            assertNotEquals(
                "$f 用了部署失敗的標題，但它連引擎都沒起來",
                R.string.not_ready_failed,
                failedTitleRes(f),
            )
        }
    }

    /* ─────────────── 3. 全覆蓋 ─────────────── */

    /**
     * 日後多一種失敗種類時，這一條逼人回答「它給不給按鈕、講什麼」。
     *
     * `when` 沒有 else 分支，所以少一種是編譯錯誤；這裡補的是「有寫，但寫成
     * 跟別人一樣」—— 除了 NONE 刻意與 RIME_INIT 共用（都是「不知道，重開看看」），
     * 其餘每一種都必須有自己的話。
     */
    @Test
    fun `每一種失敗種類都被回答過`() {
        for (f in kinds) {
            assertTrue("$f 沒有文案", failedBodyRes(f) != 0)
            assertTrue("$f 沒有標題", failedTitleRes(f) != 0)
        }
        assertEquals(
            "NONE 的意思是「不知道為什麼」，它該跟「引擎起不來」講同一句話",
            failedBodyRes(RimeRuntime.Failure.RIME_INIT),
            failedBodyRes(RimeRuntime.Failure.NONE),
        )
        assertEquals(
            "NONE 不知道為什麼，所以不能給一顆按不動的按鈕",
            NotReadyAction.NONE,
            actionOf(SetupStage.FAILED, RimeRuntime.Failure.NONE),
        )
    }
}
