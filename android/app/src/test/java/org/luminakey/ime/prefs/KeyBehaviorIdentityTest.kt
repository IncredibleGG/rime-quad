package org.luminakey.ime.prefs

import androidx.compose.ui.platform.ViewConfiguration
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Test
import org.luminakey.ime.theme.Feedback
import org.luminakey.ime.theme.HapticStrength

/**
 * 迴歸測試：提供給 CompositionLocal 的行為物件必須有**值相等性**。
 *
 * ── 為什麼這件事會變成一個視覺 bug ──────────────────────────────────────
 * `RimeInputMethodService` 的 `setContent` lambda 每一次 `uiState` 變動都會
 * 重跑（也就是每按一顆鍵），並在裡面把 `LocalViewConfiguration` 換成一份
 * `LongPressViewConfiguration`。那個類別當初沒有 `equals`，於是每次都是
 * 「新值」。
 *
 * Compose 對 `LocalViewConfiguration` 換值的反應是：對整棵樹的每一個
 * `Modifier.pointerInput` 呼叫 `onViewConfigurationChange()`，
 * 而那支的實作是 `resetPointerInputHandler()` —— 把**手指還按著的那顆鍵**
 * 的手勢協程當場砍掉。使用者看到的是「點一下鍵就變灰，變不回來」。
 *
 * 所以這不是「順手加個 equals 比較乾淨」，是這兩個類別的相等性語義
 * 直接決定了鍵盤按起來對不對。
 *
 * 上游還有第二層保險（服務端的 `remember`），下游還有第三層
 * （[org.luminakey.ime.keyboard.trackPressed] 的 `finally`）。
 */
class KeyBehaviorIdentityTest {

    private val base = FakeViewConfiguration()

    @Test
    fun sameLongPressThresholdMeansSameViewConfiguration() {
        assertEquals(
            LongPressViewConfiguration(base, 500L),
            LongPressViewConfiguration(base, 500L),
        )
        assertEquals(
            LongPressViewConfiguration(base, 500L).hashCode(),
            LongPressViewConfiguration(base, 500L).hashCode(),
        )
    }

    @Test
    fun aDifferentThresholdIsADifferentViewConfiguration() {
        assertNotEquals(
            LongPressViewConfiguration(base, 500L),
            LongPressViewConfiguration(base, 800L),
        )
    }

    @Test
    fun theOverriddenValueIsStillTheOnlyThingThatChanges() {
        val vc = LongPressViewConfiguration(base, 777L)
        assertEquals(777L, vc.longPressTimeoutMillis)
        assertEquals(base.doubleTapTimeoutMillis, vc.doubleTapTimeoutMillis)
        assertEquals(base.touchSlop, vc.touchSlop, 0f)
    }

    @Test
    fun sameThemeAndPrefsMeanSameKeyBehavior() {
        val feedback = Feedback(
            haptic = true,
            hapticStrength = HapticStrength.MEDIUM,
            sound = false,
            soundVolume = 0.3f,
        )
        val prefs = UserPrefs()
        assertEquals(KeyBehavior.of(feedback, prefs), KeyBehavior.of(feedback, prefs))
        assertEquals(
            KeyBehavior.of(feedback, prefs).hashCode(),
            KeyBehavior.of(feedback, prefs).hashCode(),
        )
    }

    @Test
    fun aChangedPreferenceIsStillADifferentKeyBehavior() {
        val feedback = Feedback(
            haptic = true,
            hapticStrength = HapticStrength.MEDIUM,
            sound = false,
            soundVolume = 0.3f,
        )
        assertNotEquals(
            KeyBehavior.of(feedback, UserPrefs()),
            KeyBehavior.of(feedback, UserPrefs(longPressMs = 900)),
        )
        assertNotEquals(
            KeyBehavior.of(feedback, UserPrefs()),
            KeyBehavior.of(feedback.copy(haptic = false), UserPrefs()),
        )
    }
}

private class FakeViewConfiguration : ViewConfiguration {
    override val longPressTimeoutMillis: Long = 400L
    override val doubleTapTimeoutMillis: Long = 300L
    override val doubleTapMinTimeMillis: Long = 40L
    override val touchSlop: Float = 8f
}
