package org.luminakey.ime.home

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * [actionOf] / [failedBodyRes] / [failedTitleRes] 與 [SetupStage.FAILED]
 * **真的接到畫面上**，而且**資料真的流過去**。
 *
 * ── 為什麼需要這一份，[SetupStageTest] 與 [FailedAdviceTest] 不夠嗎 ──────
 * 不夠。純函式全綠、而畫面上沒有人呼叫它，是這個缺陷最可能的復發方式 ——
 * 原本那一行 `if (stage == NOT_ENABLED || stage == ENABLED_NOT_DEFAULT)` 就住在
 * Composable 裡，沒有任何測試看得見它。這份掃描守的是「決定權還在純函式手上」。
 *
 * ── ⚠ 上一版這裡守得太鬆 ───────────────────────────────────────────────
 * 上一版的規則是 `src.contains("actionOf(")` 這種**整檔案掃一個字串**。
 * 那擋不住「有人呼叫了 `actionOf(stage)` 卻沒有把 `failure` 傳進去」——
 * 而那正是這一輪要修的缺陷本身。所以本版一律驗**呼叫形狀**：
 *
 *   · `actionOf(stage, failure)` —— 兩個參數都在，不是只有 stage；
 *   · `failedBodyRes(failure)` / `failedTitleRes(failure)` —— 參數真的是 failure；
 *   · `NotReadyStrip(… failure = failure …)`、`FailedBody(… failure = failure …)`
 *     —— 值真的從上層流下來，不是在裡面自己讀 `RimeRuntime.failure`
 *     （那是 `@Volatile` 欄位，Compose 讀了不會登記，畫不出來）；
 *   · `rememberRimeStatus()` —— 而且**不可以**再出現
 *     `remember(phase) { RimeRuntime.initError }`（第二次失敗會停在舊訊息）。
 *
 * ── 它抓不到什麼（誠實說明）──────────────────────────────────────────────
 * 這是**文字比對**，不是跑一次畫面。它證明得了「呼叫點的形狀對」，
 * 證明不了那顆按鈕真的畫得出來、位置對不對、字通不通順。那一層要人看。
 * 這份的價值在於：**把它拆掉就會紅**，而「拆掉」正是上一版發生過的事。
 */
class NotReadyUiWiringTest {

    /* ─────────────── 1. 真的掃一次 ─────────────── */

    @Test
    fun `首頁的失敗態接上了決策函式與資料`() {
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

    @Test
    fun `失敗訊息不再拿 phase 當快取鍵`() {
        val src = read("ImeSetupState.kt")
        assertTrue("ImeSetupState.kt 只讀到 ${src.length} 個字元", src.length >= MIN_CHARS)
        assertEquals("引擎狀態的接線退化了", emptyList<String>(), statusProblems(src))
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
            HOME_RULES,
            homeProblems(old).size,
        )
    }

    /**
     * **這一輪的重點反向測試**：五條失敗路徑一視同仁的那一版。
     *
     * 它有 `actionOf(`、有 `initError`、有 `SetupStage.FAILED`、有那顆按鈕的
     * 文案 —— 上一版的四條規則**一條都不會叫**。這一版必須叫。
     */
    @Test
    fun `只傳 stage 不傳 failure 的寫法會被抓到`() {
        val old = """
            val initError = rememberRimeInitError(phase)
            NotReadyStrip(stage = stage, system = system, initError = initError)
            SetupStage.FAILED -> stringResource(R.string.not_ready_failed)
            SetupStage.FAILED -> stringResource(R.string.not_ready_failed_body)
            val action = actionOf(stage)
            NotReadyAction.OPEN_IME_SETTINGS -> R.string.not_ready_refresh_words
        """.trimIndent()
        val problems = homeProblems(old)
        assertTrue(
            "只傳 stage 的寫法沒有被抓到 —— 那正是這一輪要修的缺陷本身",
            problems.any { it.contains("actionOf") },
        )
        assertTrue("沒抓到 failure 沒有流下去", problems.any { it.contains("failure = failure") })
        assertTrue("沒抓到文案沒有分種類", problems.any { it.contains("failedBodyRes") })
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
        assertEquals(ONBOARDING_RULES, onboardingProblems(old).size)
    }

    /** 上一版那條「拿 phase 當快取鍵」的寫法。 */
    @Test
    fun `上一版的狀態接線會被抓到`() {
        val old = """
            @Composable
            fun rememberRimeInitError(phase: RimeRuntime.Phase): String? =
                remember(phase) { RimeRuntime.initError }

            @Composable
            fun rememberRimePhase(): RimeRuntime.Phase {
                var phase by remember { mutableStateOf(RimeRuntime.phase) }
                DisposableEffect(Unit) {
                    val listener: (RimeRuntime.Phase) -> Unit = { phase = it }
                    RimeRuntime.addListener(listener)
                    onDispose { RimeRuntime.removeListener(listener) }
                }
                return phase
            }
        """.trimIndent()
        assertEquals(STATUS_RULES, statusProblems(old).size)
    }

    /** 反向測試的另一半：**接對了不可以叫**。會亂叫的檢查一樣會被關掉。 */
    @Test
    fun `接對了不算問題`() {
        val good = """
            val status = rememberRimeStatus()
            val initError = status.initError
            NotReadyStrip(stage = stage, initError = initError, failure = failure)
            SetupStage.FAILED -> stringResource(failedTitleRes(failure))
            SetupStage.FAILED -> stringResource(failedBodyRes(failure))
            val action = actionOf(stage, failure)
            NotReadyAction.SWITCH_IME -> R.string.not_ready_refresh_words
        """.trimIndent()
        assertEquals(emptyList<String>(), homeProblems(good))

        val goodOnboarding = """
            SetupStage.FAILED -> FailedBody(error = initError, failure = failure)
            private fun PreparingBody(phase: RimeRuntime.Phase) { }
            if (actionOf(SetupStage.FAILED, failure) == NotReadyAction.REFRESH_WORDS) {
                Button(onClick = onRefreshWords) { }
            }
            Text(text = stringResource(failedBodyRes(failure)))
        """.trimIndent()
        assertEquals(emptyList<String>(), onboardingProblems(goodOnboarding))

        val goodStatus = """
            @Composable
            fun rememberRimeStatus(): RimeRuntime.Status {
                var status by remember { mutableStateOf(RimeRuntime.status()) }
                DisposableEffect(Unit) {
                    val listener: (RimeRuntime.Phase) -> Unit = { status = RimeRuntime.status() }
                    RimeRuntime.addListener(listener)
                    onDispose { RimeRuntime.removeListener(listener) }
                }
                return status
            }
        """.trimIndent()
        assertEquals(emptyList<String>(), statusProblems(goodStatus))
    }

    /* ─────────────── 規則本體 ─────────────── */

    private fun homeProblems(raw: String): List<String> = buildList {
        val src = strip(raw)
        if (!src.contains("initError")) {
            add("首頁沒有任何地方讀 initError —— 失敗態帶不出原因")
        }
        // ⚠ 驗的是**兩個參數的呼叫**，不是「檔案裡有 actionOf 這幾個字」。
        if (!ACTION_OF_WITH_FAILURE.containsMatchIn(src)) {
            add(
                "首頁沒有 actionOf(stage, failure) —— 少了 failure，" +
                    "四條按不動的失敗路徑又會畫出「重新整理字詞」"
            )
        }
        if (!src.contains("SetupStage.FAILED")) {
            add("首頁的 when(stage) 沒有 FAILED 這一格，失敗會被畫成準備中")
        }
        if (!src.contains("R.string.not_ready_refresh_words")) {
            add("首頁沒有「重新整理字詞」那顆按鈕的文案 —— 部署失敗時沒有出路")
        }
        if (!FAILED_BODY_OF_FAILURE.containsMatchIn(src)) {
            add("首頁沒有 failedBodyRes(failure) —— 五種失敗會共用同一句話")
        }
        if (!FAILED_TITLE_OF_FAILURE.containsMatchIn(src)) {
            add("首頁沒有 failedTitleRes(failure) —— 引擎沒起來也會說「字詞整理沒成功」")
        }
        if (!PASSES_FAILURE.containsMatchIn(src)) {
            add("failure = failure 沒有往下傳 —— 橫幅拿不到失敗種類")
        }
    }

    private fun onboardingProblems(raw: String): List<String> = buildList {
        val src = strip(raw)
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
        if (!PASSES_FAILURE.containsMatchIn(src)) {
            add("failure = failure 沒有傳進 FailedBody —— 那一屏拿不到失敗種類")
        }
        if (!ACTION_OF_WITH_FAILURE.containsMatchIn(src)) {
            add(
                "引導頁沒有用 actionOf(…, failure) 決定畫不畫那顆按鈕 —— " +
                    "寫在 Composable 裡的 if 沒有人驗得到"
            )
        }
        if (!FAILED_BODY_OF_FAILURE.containsMatchIn(src)) {
            add("引導頁沒有 failedBodyRes(failure) —— 五種失敗會共用同一句話")
        }
    }

    private fun statusProblems(raw: String): List<String> = buildList {
        val src = strip(raw)
        if (STALE_ERROR_KEY.containsMatchIn(src)) {
            add(
                "失敗訊息又拿 phase 當 remember 的鍵了 —— 第二次失敗時 phase 還是 FAILED，" +
                    "structural equality 不觸發重組，畫面會停在上一次的錯誤"
            )
        }
        if (!src.contains("rememberRimeStatus")) {
            add("沒有 rememberRimeStatus() —— 訊息與失敗種類沒有載體")
        }
        if (!LISTENER_TAKES_SNAPSHOT.containsMatchIn(src)) {
            add(
                "回呼裡沒有重新取整包快照（RimeRuntime.status()）—— " +
                    "只把回呼帶進來的 phase 存起來，等於又回到只看 phase"
            )
        }
    }

    /**
     * ⚠ 註解一定要先砍掉。
     *
     * 這不是預防性的：`ImeSetupState.kt` 的 KDoc 裡就**逐字寫著**
     * `remember(phase) { RimeRuntime.initError }`（在解釋為什麼不能那樣寫）。
     * 不砍註解的話，這份守門會對著一段在教人別那樣寫的說明大叫，
     * 然後被關掉 —— 一個會亂叫的檢查比沒有更糟。
     */
    private fun strip(src: String): String =
        src.lineSequence().joinToString("\n") { line ->
            val i = line.indexOf("//")
            if (i >= 0 && line.take(i).isBlank()) "" else line
        }.replace(BLOCK_COMMENT, "")

    private fun read(name: String): String {
        val f = File(homeRoot, name)
        assertTrue("找不到 ${f.path}", f.isFile)
        return f.readText()
    }

    companion object {
        /** 單元測試的工作目錄是模組目錄（`android/app`）。 */
        private val homeRoot = File("src/main/java/org/luminakey/ime/home")

        /** 三支檔案都是好幾千字元；讀到比這個少一定是路徑錯了。 */
        private const val MIN_CHARS = 3000

        /** [homeProblems] / [onboardingProblems] / [statusProblems] 的規則條數。 */
        private const val HOME_RULES = 7
        private const val ONBOARDING_RULES = 6
        private const val STATUS_RULES = 3

        private val BLOCK_COMMENT = Regex("""/\*[\s\S]*?\*/""")

        private val ZERO_ARG_PREPARING = Regex("""fun PreparingBody\(\s*\)""")

        /**
         * `actionOf(<某物>, failure)` —— 第二個參數必須**就是那個 failure 變數**。
         *
         * 不能只驗「有兩個參數」：`actionOf(stage, RimeRuntime.Failure.DEPLOY)`
         * 也是兩個參數，而它等於把失敗種類寫死成「一定按得動」，
         * 四條死路又會長回那顆按鈕。
         */
        private val ACTION_OF_WITH_FAILURE = Regex("""actionOf\([^()]*,\s*failure\s*\)""")
        private val FAILED_BODY_OF_FAILURE = Regex("""failedBodyRes\(\s*failure\s*\)""")
        private val FAILED_TITLE_OF_FAILURE = Regex("""failedTitleRes\(\s*failure\s*\)""")
        private val PASSES_FAILURE = Regex("""failure\s*=\s*(status\.)?failure""")

        /**
         * 上一版那條把訊息綁死在 phase 上的寫法。
         *
         * 只要在這支檔案裡出現 `remember(phase)` 就算違規，不必看大括號裡是什麼：
         * 第二次失敗時 phase 從頭到尾都是 FAILED，拿它當鍵**不管快取什麼**都會過期。
         */
        private val STALE_ERROR_KEY = Regex("""remember\(\s*phase\s*\)""")

        /** 回呼裡要重新取整包快照，而不是只存下回呼帶進來的 phase。 */
        private val LISTENER_TAKES_SNAPSHOT = Regex("""=\s*RimeRuntime\.status\(\)""")
    }
}
