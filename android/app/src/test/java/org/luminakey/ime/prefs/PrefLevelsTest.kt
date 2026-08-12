package org.luminakey.ime.prefs

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.theme.HapticStrength

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

    /**
     * 主題的「有多少畫多少」（`max_visible: 0`）落在最後一檔。
     *
     * 那一檔以前叫「不限」，現在叫「5 個」—— 因為引擎一頁就是 5 個，
     * 「不限」與「5 個」在畫面上是同一件事，而前者會讓使用者以為還能更多。
     */
    @Test
    fun unlimitedCandidateCountIsTheLastStep() {
        val last = PrefLevels.CANDIDATE_COUNT_LABELS.size - 1
        assertEquals(last, PrefLevels.indexOfCandidateCount(UserPrefs(), baseCount = 0))
        assertEquals(last, PrefLevels.indexOfCandidateCount(UserPrefs(), baseCount = 5))
    }

    /* ── 死檔位：一個按了沒反應的設定比沒有這個設定更糟 ── */

    /**
     * **每一檔都必須畫得出不同的數量。**
     *
     * 改動前這裡有 3 / 5 / 7 / 9 / 不限五檔，而引擎一頁只給
     * [PrefLevels.ENGINE_PAGE_SIZE] 個（`core/data/shared/default.yaml` 的
     * `menu/page_size: 5`；實測 emulator-5558 打 `zongguo` 回「候選 5 個」）。
     * 於是 7 / 9 / 不限三檔按下去畫面**一模一樣** —— 三個死檔位。
     *
     * 這一條同時是那三檔的墓碑：把它們加回來就會紅。
     */
    @Test
    fun everyCandidateCountStepChangesSomething() {
        val caps = PrefLevels.CANDIDATE_COUNT_LABELS.indices.map { i ->
            PrefLevels.withCandidateCount(UserPrefs(), i).candidateCount
        }
        assertEquals("每一檔的值必須互不相同", caps.distinct().size, caps.size)
        for (cap in caps) {
            assertTrue(
                "$cap 超過引擎一頁的 ${PrefLevels.ENGINE_PAGE_SIZE} 個 —— 那一檔按了不會有反應",
                cap != null && cap in 1..PrefLevels.ENGINE_PAGE_SIZE,
            )
        }
    }

    /** 舊使用者存過的 7 / 9 / 0 不會讓面板指到一個不存在的檔位。 */
    @Test
    fun legacyCandidateCountsLandOnTheLastStep() {
        val last = PrefLevels.CANDIDATE_COUNT_LABELS.size - 1
        for (old in listOf(0, 7, 9, 99)) {
            assertEquals(
                "舊值 $old 應該落在最後一檔",
                last,
                PrefLevels.indexOfCandidateCount(UserPrefs(candidateCount = old), baseCount = 0),
            )
        }
    }

    @Test
    fun nearestPicksTheClosestStep() {
        val steps = listOf(0f, 1f, 2f)
        assertEquals(0, PrefLevels.nearest(steps, -5f))
        assertEquals(2, PrefLevels.nearest(steps, 9f))
        assertEquals(1, PrefLevels.nearest(steps, 1.4f))
    }
}
