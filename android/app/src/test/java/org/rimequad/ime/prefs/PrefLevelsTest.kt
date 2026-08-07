package org.rimequad.ime.prefs

import org.junit.Assert.assertEquals
import org.junit.Test
import org.rimequad.ime.theme.HapticStrength

/**
 * 檔位對照表。
 *
 * 兩個消費端（App 的第二層設定頁、鍵盤上的就地編輯器）共用同一份表，
 * 所以「選了『中』之後再讀回來還是『中』」這條往返性質必須成立 ——
 * 不成立的話，使用者在鍵盤上調完、回到 App 會看到另一個檔位被選著。
 */
class PrefLevelsTest {

    @Test
    fun soundRoundTrips() {
        for (i in PrefLevels.SOUND_LABELS.indices) {
            val p = PrefLevels.withSound(UserPrefs(), i)
            assertEquals(
                "按鍵音第 $i 檔存回去再讀出來不一樣",
                i,
                PrefLevels.indexOfSound(p, baseEnabled = false, baseVolume = 0f),
            )
        }
    }

    @Test
    fun hapticRoundTrips() {
        for (i in PrefLevels.HAPTIC_LABELS.indices) {
            val p = PrefLevels.withHaptic(UserPrefs(), i)
            assertEquals(
                i,
                PrefLevels.indexOfHaptic(p, baseEnabled = false, baseStrength = HapticStrength.NONE),
            )
        }
    }

    @Test
    fun longPressRoundTrips() {
        for (i in PrefLevels.LONG_PRESS_LABELS.indices) {
            assertEquals(i, PrefLevels.indexOfLongPress(PrefLevels.withLongPress(UserPrefs(), i)))
        }
    }

    @Test
    fun candidateCountRoundTrips() {
        for (i in PrefLevels.CANDIDATE_COUNT_LABELS.indices) {
            val p = PrefLevels.withCandidateCount(UserPrefs(), i)
            assertEquals(i, PrefLevels.indexOfCandidateCount(p, baseCount = 0))
        }
    }

    @Test
    fun candidateSizeRoundTrips() {
        for (i in PrefLevels.CANDIDATE_SIZE_LABELS.indices) {
            assertEquals(
                i,
                PrefLevels.indexOfCandidateSize(PrefLevels.withCandidateSize(UserPrefs(), i)),
            )
        }
    }

    /* ── 「未設定」要反推自主題檔的值，不能一律顯示成第 0 檔 ── */

    @Test
    fun unsetFallsBackToTheThemeValue() {
        // 主題說「有聲音、音量 0.65」→ 面板上該亮的是「中」，不是「關」。
        assertEquals(
            2,
            PrefLevels.indexOfSound(UserPrefs(), baseEnabled = true, baseVolume = 0.65f),
        )
        // 主題說沒有聲音 → 「關」。
        assertEquals(
            0,
            PrefLevels.indexOfSound(UserPrefs(), baseEnabled = false, baseVolume = 0.65f),
        )
        assertEquals(
            3,
            PrefLevels.indexOfHaptic(UserPrefs(), true, HapticStrength.HEAVY),
        )
    }

    @Test
    fun themeValuesBetweenStepsSnapToTheNearestOne() {
        // 主題可以寫任何值；面板只有四檔，落在檔位之間時取最近的一檔。
        // 顯示成一個檔位不精確，但總比顯示成「關」好 —— 那是**錯的**。
        assertEquals(1, PrefLevels.indexOfSound(UserPrefs(), baseEnabled = true, baseVolume = 0.33f))
        assertEquals(3, PrefLevels.indexOfSound(UserPrefs(), baseEnabled = true, baseVolume = 0.97f))
    }

    @Test
    fun unlimitedCandidateCountIsTheLastStep() {
        assertEquals(4, PrefLevels.indexOfCandidateCount(UserPrefs(), baseCount = 0))
        assertEquals(1, PrefLevels.indexOfCandidateCount(UserPrefs(), baseCount = 5))
    }

    @Test
    fun nearestPicksTheClosestStep() {
        val steps = listOf(0f, 1f, 2f)
        assertEquals(0, PrefLevels.nearest(steps, -5f))
        assertEquals(2, PrefLevels.nearest(steps, 9f))
        assertEquals(1, PrefLevels.nearest(steps, 1.4f))
    }
}
