package org.luminakey.ime.home

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.core.RimeRuntime

/**
 * 引導頁的判斷邏輯。
 *
 * 這幾條看起來瑣碎，但每一條都對應一個真的會把使用者困住的缺陷：
 *   · 比對完整 IME id → service 類別改名之後永遠回不了「已完成」；
 *   · 只有三種狀態 → 使用者在部署那十幾秒裡點試打框、打不出字、認定壞掉。
 */
class SetupStageTest {

    private val me = "org.luminakey.ime"

    /* ── 一律比對套件名，不比對完整 id ── */

    @Test
    fun defaultImeIsMatchedByPackageNotByFullId() {
        // 完整 id 長這樣（實測 Settings.Secure.DEFAULT_INPUT_METHOD 的回傳）。
        assertTrue(isDefaultImePackage("org.luminakey.ime/.RimeInputMethodService", me))
        // service 類別日後被重構到別的路徑，仍然必須認得出來 ——
        // 比對完整 id 的實作在這裡就會把使用者鎖在引導頁裡。
        assertTrue(isDefaultImePackage("org.luminakey.ime/org.luminakey.ime.ime.NewService", me))
        assertFalse(
            isDefaultImePackage(
                "com.google.android.inputmethod.latin/com.android.inputmethod.latin.LatinIME",
                me,
            )
        )
    }

    @Test
    fun missingDefaultImeIsNotUs() {
        assertFalse(isDefaultImePackage(null, me))
        assertFalse(isDefaultImePackage("", me))
    }

    @Test
    fun packageOfImeIdHandlesJunk() {
        assertEquals("a.b", packageOfImeId("a.b/c"))
        assertEquals("a.b", packageOfImeId("a.b"))
        assertEquals(null, packageOfImeId(null))
        assertEquals(null, packageOfImeId("/x"))
    }

    @Test
    fun enabledIsAPackageMembershipTest() {
        assertTrue(isEnabledPackage(listOf("com.other", me), me))
        assertFalse(isEnabledPackage(listOf("com.other"), me))
        assertFalse(isEnabledPackage(emptyList(), me))
    }

    /* ── 四種狀態 ── */

    private fun state(enabled: Boolean, default: Boolean) =
        ImeSystemState(enabled = enabled, isDefault = default, currentImeLabel = null)

    @Test
    fun notEnabledWinsOverEverything() {
        // 沒啟用時，部署跑完了也還是「沒啟用」。
        assertEquals(
            SetupStage.NOT_ENABLED,
            stageOf(state(enabled = false, default = false), RimeRuntime.Phase.READY),
        )
    }

    @Test
    fun enabledButNotDefault() {
        assertEquals(
            SetupStage.ENABLED_NOT_DEFAULT,
            stageOf(state(enabled = true, default = false), RimeRuntime.Phase.READY),
        )
    }

    @Test
    fun defaultButStillDeployingIsNotReady() {
        // **這一條是本檔存在的理由。** 「已啟用 + 已是預設」不等於「能打字」：
        // 首次部署的實測耗時見 core/DeployEstimate.kt。
        assertEquals(
            SetupStage.PREPARING,
            stageOf(state(enabled = true, default = true), RimeRuntime.Phase.DEPLOYING),
        )
        assertEquals(
            SetupStage.PREPARING,
            stageOf(state(enabled = true, default = true), RimeRuntime.Phase.EXTRACTING),
        )
    }

    /**
     * **失敗不是「還在準備」。**
     *
     * 併成同一格的版本出過貨，後果是首頁永遠停在「正在整理字詞」而且一顆按鈕
     * 都沒有 —— 那一格刻意不給按鈕（「沒有人能加速它」），而那句話對已經失敗
     * 的情況是錯的。所以這裡不只斷言「不是 READY」，還要斷言「不是 PREPARING」。
     */
    @Test
    fun failedIsItsOwnStage() {
        val stage = stageOf(state(enabled = true, default = true), RimeRuntime.Phase.FAILED)
        assertEquals(SetupStage.FAILED, stage)
        assertNotEquals("把失敗畫成等待，使用者會一直等下去", SetupStage.PREPARING, stage)
        assertNotEquals("把失敗當成可用是最不能犯的錯", SetupStage.READY, stage)
    }

    /**
     * 系統那兩步仍然排在最前面。
     *
     * 這不是漏寫，是選擇：那兩步是前置條件，字詞整理好了他還是打不了字。
     * 記在測試裡，日後有人想改順序時會先讀到這一條。
     */
    @Test
    fun systemStepsStillComeFirstWhenDeployFailed() {
        assertEquals(
            SetupStage.NOT_ENABLED,
            stageOf(state(enabled = false, default = false), RimeRuntime.Phase.FAILED),
        )
        assertEquals(
            SetupStage.ENABLED_NOT_DEFAULT,
            stageOf(state(enabled = true, default = false), RimeRuntime.Phase.FAILED),
        )
    }

    /* ── 哪一格給按鈕 ── */

    /** 失敗態一定要有一條看得見的自救路徑。 */
    @Test
    fun failedStageOffersAWayOut() {
        assertEquals(NotReadyAction.REFRESH_WORDS, actionOf(SetupStage.FAILED))
    }

    /** 準備中真的沒有人能加速它 —— 給一顆按鈕只會讓人按了以為有用。 */
    @Test
    fun preparingAndReadyHaveNoButton() {
        assertEquals(NotReadyAction.NONE, actionOf(SetupStage.PREPARING))
        assertEquals(NotReadyAction.NONE, actionOf(SetupStage.READY))
    }

    /**
     * 全覆蓋：除了「還在跑」與「好了」以外的每一格都必須有出路。
     *
     * 日後多加一格而忘了決定要不要給按鈕時，這一條會紅 —— 而不是安靜地
     * 多出一個沒有按鈕的死路。
     */
    @Test
    fun everyBlockedStageHasSomethingToDo() {
        for (s in SetupStage.values()) {
            if (s == SetupStage.PREPARING || s == SetupStage.READY) continue
            assertNotEquals("$s 這一格沒有任何出路", NotReadyAction.NONE, actionOf(s))
        }
    }

    @Test
    fun ready() {
        assertEquals(
            SetupStage.READY,
            stageOf(state(enabled = true, default = true), RimeRuntime.Phase.READY),
        )
    }
}
