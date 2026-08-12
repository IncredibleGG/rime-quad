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
     * 「不限」這個**名字**沒有回來:`max_visible: 0` 畫出來就是一整頁,而
     * 一整頁正好是最後一檔,兩者在畫面上是同一件事 —— 而「不限」會讓使用者
     * 以為還能更多。
     *
     * ⚠ 這一條就是那個真實缺陷的守門。合併之後預設仍然是「不限」(0),
     *   但檔位只到 5,於是設定列**顯示成「5 個」**而畫面上其實有 9 個;
     *   使用者只要碰它一下就寫進 5,永久鎖住、回不到 9。
     *   最後一檔等於一整頁,這條路才走得通,所以這裡拿
     *   [PrefLevels.ENGINE_PAGE_SIZE] 當基準,不是寫死的數字。
     */
    @Test
    fun unlimitedCandidateCountIsTheLastStep() {
        val last = PrefLevels.CANDIDATE_COUNT_LABELS.size - 1
        assertEquals(last, PrefLevels.indexOfCandidateCount(UserPrefs(), baseCount = 0))
        assertEquals(
            last,
            PrefLevels.indexOfCandidateCount(UserPrefs(), baseCount = PrefLevels.ENGINE_PAGE_SIZE),
        )
        // 而「碰一下」之後存進去的值,必須就是它本來畫得出來的那個數量 ——
        // 不可以比原本少。這一步是使用者實際會做的動作。
        assertEquals(
            "碰一下設定列就把候選數調降了,而使用者沒有要求任何改變",
            PrefLevels.ENGINE_PAGE_SIZE,
            PrefLevels.withCandidateCount(UserPrefs(), last).candidateCount,
        )
    }

    /* ── 死檔位：一個按了沒反應的設定比沒有這個設定更糟 ── */

    /**
     * **每一檔都必須畫得出不同的數量。**
     *
     * 這裡一度有 3 / 5 / 7 / 9 / 不限五檔，而當時引擎一頁只給 5 個
     * （`core/data/shared/default.yaml` 的 `menu/page_size: 5`）。
     * 於是 7 / 9 / 不限三檔按下去畫面**一模一樣** —— 三個死檔位,砍成 3/4/5。
     *
     * 資料後來把 `page_size` 改成 9,砍掉的理由就沒了。所以這一條守的不是
     * 某一組數字,是**那條關係**:每一檔都必須落在引擎那一頁之內。
     * 上限跟著 [PrefLevels.ENGINE_PAGE_SIZE] 走,而那個常數跟著資料走。
     *
     * 「最後一檔正好等於引擎那一頁」由 [EnginePageSizeTest] 守另一半。
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
        // 檔位要遞增。亂序的話「往右調 = 更多」這個唯一的線索就沒了。
        assertEquals("檔位必須由小到大", caps.filterNotNull().sorted(), caps.filterNotNull())
    }

    /**
     * 大於一整頁的舊值落在最後一檔。
     *
     * ⚠ 這裡**刻意不再包含 7**。7 現在是一個真的檔位（引擎一頁給 9 個），
     * 舊使用者存的 7 應該回到「7 個」那一格,而不是被推到最後一檔 ——
     * 把他選過的數字改掉,和從前那個「顯示成一個從來沒生效過的數字」
     * 是同一類謊,只是方向相反。7 的落點由
     * [legacyCandidateCountSevenIsARealStepAgain] 守。
     */
    @Test
    fun legacyCandidateCountsLandOnTheLastStep() {
        val last = PrefLevels.CANDIDATE_COUNT_LABELS.size - 1
        for (old in listOf(0, PrefLevels.ENGINE_PAGE_SIZE, 99)) {
            assertEquals(
                "舊值 $old 應該落在最後一檔",
                last,
                PrefLevels.indexOfCandidateCount(UserPrefs(candidateCount = old), baseCount = 0),
            )
        }
    }

    /**
     * 批 1 把 7 / 9 砍掉時，舊使用者存的 7 被推到最後一檔（當時是「5 個」）。
     * 引擎一頁變成 9 個之後 7 又畫得出來了，那些人應該拿回自己選過的數字。
     */
    @Test
    fun legacyCandidateCountSevenIsARealStepAgain() {
        val i = PrefLevels.indexOfCandidateCount(UserPrefs(candidateCount = 7), baseCount = 0)
        assertEquals(
            "存過 7 的使用者應該看到「7 個」,而且它真的畫得出 7 個",
            7,
            PrefLevels.withCandidateCount(UserPrefs(), i).candidateCount,
        )
    }

    @Test
    fun nearestPicksTheClosestStep() {
        val steps = listOf(0f, 1f, 2f)
        assertEquals(0, PrefLevels.nearest(steps, -5f))
        assertEquals(2, PrefLevels.nearest(steps, 9f))
        assertEquals(1, PrefLevels.nearest(steps, 1.4f))
    }
}
