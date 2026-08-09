package org.luminakey.ime.home

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * [actionOf] 與 [SetupStage.FAILED] **真的接到畫面上**。
 *
 * ── 為什麼需要這一份，[SetupStageTest] 不夠嗎 ────────────────────────────
 * 不夠。純函式全綠、而畫面上沒有人呼叫它，是這個缺陷最可能的復發方式 ——
 * 原本那一行 `if (stage == NOT_ENABLED || stage == ENABLED_NOT_DEFAULT)` 就住在
 * Composable 裡，沒有任何測試看得見它。這份掃描守的是「決定權還在純函式手上」。
 *
 * ── 它抓不到什麼（誠實說明）──────────────────────────────────────────────
 * 這是**文字比對**，不是跑一次畫面。它證明得了「AppScreen.kt 呼叫了 actionOf、
 * 讀了 initError、有 FAILED 這一格」，證明不了那顆按鈕真的畫得出來、位置對不對、
 * 字通不通順。那一層要人看。這份的價值在於：**把它拆掉就會紅**，
 * 而「拆掉」正是上一版發生過的事。
 */
class NotReadyUiWiringTest {

    /* ─────────────── 1. 真的掃一次 ─────────────── */

    @Test
    fun `首頁的失敗態接上了 actionOf 與 initError`() {
        val src = read("AppScreen.kt")
        // G2：範圍非空 —— 路徑寫錯時必須是紅，不是零個問題。
        assertTrue("AppScreen.kt 只讀到 ${src.length} 個字元，路徑大概錯了", src.length >= MIN_CHARS)
        assertTrue(
            "AppScreen.kt 裡找不到 NotReadyStrip —— 這條測試已經對不上實作了",
            src.contains("NotReadyStrip"),
        )
        assertEquals("首頁那條「還不能用」的橫幅沒有接好", emptyList<String>(), homeProblems(src))
    }

    @Test
    fun `引導頁畫得出失敗那一屏`() {
        val src = read("Onboarding.kt")
        assertTrue("Onboarding.kt 只讀到 ${src.length} 個字元，路徑大概錯了", src.length >= MIN_CHARS)
        assertTrue(
            "Onboarding.kt 裡找不到 PreparingBody —— 這條測試已經對不上實作了",
            src.contains("PreparingBody"),
        )
        assertEquals("引導頁的失敗那一屏沒有接好", emptyList<String>(), onboardingProblems(src))
    }

    /* ─────────────── 2. 反向測試（G1）─────────────── */

    /** 餵上一版真正的寫法，確認每一條都會被抓到。沒做過這一步的檢查一律當作沒有。 */
    @Test
    fun `上一版的首頁寫法會被抓到`() {
        val old = """
            @Composable
            private fun NotReadyStrip(stage: SetupStage, system: ImeSystemState) {
                if (stage == SetupStage.NOT_ENABLED || stage == SetupStage.ENABLED_NOT_DEFAULT) {
                    PrimaryWide(text = stringResource(R.string.not_ready_open_settings)) { }
                }
            }
        """.trimIndent()
        assertEquals(
            "上一版的寫法沒有被抓到 —— 那上面那條全綠不代表任何事",
            4,
            homeProblems(old).size,
        )
    }

    @Test
    fun `上一版的引導頁寫法會被抓到`() {
        val old = """
            @Composable
            private fun PreparingBody() {
                val failed = RimeRuntime.phase == RimeRuntime.Phase.FAILED
                val error = RimeRuntime.initError
            }
        """.trimIndent()
        assertEquals(3, onboardingProblems(old).size)
    }

    /** 反向測試的另一半：**接對了不可以叫**。會亂叫的檢查一樣會被關掉。 */
    @Test
    fun `接對了不算問題`() {
        val good = """
            val initError = rememberRimeInitError(phase)
            val action = actionOf(stage)
            SetupStage.FAILED -> stringResource(R.string.not_ready_failed)
            NotReadyAction.SWITCH_IME -> R.string.not_ready_refresh_words
        """.trimIndent()
        assertEquals(emptyList<String>(), homeProblems(good))

        val goodOnboarding = """
            SetupStage.FAILED -> FailedBody(error = initError)
            private fun PreparingBody(phase: RimeRuntime.Phase) { }
        """.trimIndent()
        assertEquals(emptyList<String>(), onboardingProblems(goodOnboarding))
    }

    /* ─────────────── 規則本體 ─────────────── */

    private fun homeProblems(src: String): List<String> = buildList {
        if (!src.contains("initError")) {
            add("首頁沒有任何地方讀 initError —— 失敗態帶不出原因")
        }
        if (!src.contains("actionOf(")) {
            add("首頁沒有呼叫 actionOf() —— 該不該給按鈕又變成寫在 Composable 裡的 if 了")
        }
        if (!src.contains("SetupStage.FAILED")) {
            add("首頁的 when(stage) 沒有 FAILED 這一格，失敗會被畫成準備中")
        }
        if (!src.contains("R.string.not_ready_refresh_words")) {
            add("首頁沒有「重新整理字詞」那顆按鈕的文案 —— 失敗態沒有出路")
        }
    }

    private fun onboardingProblems(src: String): List<String> = buildList {
        if (ZERO_ARG_PREPARING.containsMatchIn(src)) {
            add(
                "PreparingBody 沒有參數 —— Compose 會跳過它的重組，" +
                    "裡面讀的 @Volatile 欄位（phase / initError）一輩子畫不出來"
            )
        }
        if (!src.contains("SetupStage.FAILED")) {
            add("引導頁沒有 FAILED 這一屏")
        }
        if (!src.contains("FailedBody(")) {
            add("引導頁沒有失敗那一屏的內容 —— 使用者會被關在引導頁裡")
        }
    }

    private fun read(name: String): String {
        val f = File(homeRoot, name)
        assertTrue("找不到 ${f.path}", f.isFile)
        return f.readText()
    }

    companion object {
        /** 單元測試的工作目錄是模組目錄（`android/app`）。 */
        private val homeRoot = File("src/main/java/org/luminakey/ime/home")

        /** 兩支檔案都是好幾千字元；讀到比這個少一定是路徑錯了。 */
        private const val MIN_CHARS = 3000

        private val ZERO_ARG_PREPARING = Regex("""fun PreparingBody\(\s*\)""")
    }
}
