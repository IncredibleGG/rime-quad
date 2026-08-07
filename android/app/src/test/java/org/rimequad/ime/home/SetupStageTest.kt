package org.rimequad.ime.home

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.rimequad.ime.core.RimeRuntime

/**
 * 引導頁的判斷邏輯。
 *
 * 這幾條看起來瑣碎，但每一條都對應一個真的會把使用者困住的缺陷：
 *   · 比對完整 IME id → service 類別改名之後永遠回不了「已完成」；
 *   · 只有三種狀態 → 使用者在部署那 8–13 秒裡點試打框、打不出字、認定壞掉。
 */
class SetupStageTest {

    private val me = "org.rimequad.ime"

    /* ── 一律比對套件名，不比對完整 id ── */

    @Test
    fun defaultImeIsMatchedByPackageNotByFullId() {
        // 完整 id 長這樣（實測 Settings.Secure.DEFAULT_INPUT_METHOD 的回傳）。
        assertTrue(isDefaultImePackage("org.rimequad.ime/.RimeInputMethodService", me))
        // service 類別日後被重構到別的路徑，仍然必須認得出來 ——
        // 比對完整 id 的實作在這裡就會把使用者鎖在引導頁裡。
        assertTrue(isDefaultImePackage("org.rimequad.ime/org.rimequad.ime.ime.NewService", me))
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
        // 首次部署實測 8.0 秒（模擬器）／12.5 秒（S24U）。
        assertEquals(
            SetupStage.PREPARING,
            stageOf(state(enabled = true, default = true), RimeRuntime.Phase.DEPLOYING),
        )
        assertEquals(
            SetupStage.PREPARING,
            stageOf(state(enabled = true, default = true), RimeRuntime.Phase.EXTRACTING),
        )
    }

    @Test
    fun failedIsNeverReady() {
        assertEquals(
            SetupStage.PREPARING,
            stageOf(state(enabled = true, default = true), RimeRuntime.Phase.FAILED),
        )
    }

    @Test
    fun ready() {
        assertEquals(
            SetupStage.READY,
            stageOf(state(enabled = true, default = true), RimeRuntime.Phase.READY),
        )
    }
}
