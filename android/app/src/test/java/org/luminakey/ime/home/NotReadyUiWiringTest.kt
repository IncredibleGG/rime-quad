package org.luminakey.ime.home

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * [actionOf] 與 [SetupStage.FAILED] **真的接到畫面上,而且按下去真的做事**。
 *
 * ── 為什麼需要這一份,[SetupStageTest] 不夠嗎 ────────────────────────────
 * 不夠。純函式全綠、而畫面上沒有人呼叫它,是這個缺陷最可能的復發方式 ——
 * 原本那一行 `if (stage == NOT_ENABLED || stage == ENABLED_NOT_DEFAULT)` 就住在
 * Composable 裡,沒有任何測試看得見它。這份掃描守的是「決定權還在純函式手上」。
 *
 * ── 上一版是假綠的,而且是被實測拆出來的 ────────────────────────────────
 * 上一版的規則寫的是 `src.contains("initError")` 與 `src.contains("FailedBody(")`。
 * 而**函式的參數宣告 `initError: String?` 與函式定義 `private fun FailedBody(`
 * 自己就含有那些字串** —— 於是覆核者實測的四種拆法一種都抓不到,568 條測試全綠:
 *
 *   (a) AppScreen.kt 失敗態按鈕的 `else -> onRefreshWords()` 改成 `else -> Unit`
 *   (b) Onboarding.kt FailedBody 的 `onClick = onRefreshWords` 改成 `onClick = {}`
 *   (c) AppScreen.kt 把整段畫 initError 的 if 區塊刪光
 *   (d) Onboarding.kt 的 `SetupStage.FAILED ->` 改回呼叫 PreparingBody
 *
 * 而 (c) 的失敗訊息還寫著「首頁沒有任何地方讀 initError」—— 讀起來比它實際
 * 驗的強得多。所以現在:
 *
 *   · 每一條判準都落在**呼叫位置與資料流**上(`initError = initError`、
 *     `text = initError`、`else -> onRefreshWords()`、`onClick = onRefreshWords`),
 *     不是「檔案裡有沒有這個字」;
 *   · 「引導頁的 FAILED 這一屏畫什麼」只在**那一格的範圍內**找,
 *     不會被三百行外的函式定義餵飽;
 *   · 而且下面有一條測試把**覆核者實測過的那四種拆法逐一植入真的原始碼**,
 *     要求每一種都被抓到。沒做過那一步的檢查一律當作沒有。
 *
 * ── 它抓不到什麼(誠實說明)──────────────────────────────────────────────
 * 這仍然是**文字比對**,不是跑一次畫面。它證明得了那幾條線接在一起,
 * 證明不了那顆按鈕真的畫得出來、位置對不對、字通不通順 —— 那一層要人看,
 * 或要 Compose UI 測試(還沒有)。它的價值在於:**把線拆掉就會紅**。
 */
class NotReadyUiWiringTest {

    /* ─────────────── 1. 真的掃一次 ─────────────── */

    @Test
    fun `首頁的失敗態接上了 actionOf、initError 與那顆按鈕`() {
        val src = read("AppScreen.kt")
        // G2:範圍非空 —— 路徑寫錯時必須是紅,不是零個問題。
        assertTrue("AppScreen.kt 只讀到 ${src.length} 個字元,路徑大概錯了", src.length >= MIN_CHARS)
        assertTrue(
            "AppScreen.kt 裡找不到 NotReadyStrip —— 這條測試已經對不上實作了",
            src.contains("NotReadyStrip"),
        )
        assertEquals("首頁那條「還不能用」的橫幅沒有接好", emptyList<String>(), homeProblems(src))
    }

    @Test
    fun `引導頁畫得出失敗那一屏,而且那顆按鈕接得到 redeploy`() {
        val src = read("Onboarding.kt")
        assertTrue("Onboarding.kt 只讀到 ${src.length} 個字元,路徑大概錯了", src.length >= MIN_CHARS)
        assertTrue(
            "Onboarding.kt 裡找不到 PreparingBody —— 這條測試已經對不上實作了",
            src.contains("PreparingBody"),
        )
        assertEquals("引導頁的失敗那一屏沒有接好", emptyList<String>(), onboardingProblems(src))
    }

    /* ─────────────── 2. 反向測試(G1)─────────────── */

    /**
     * **這一條是上一版失效的直接補丁。**
     * 覆核者實測過的四種拆法,逐一植入**真的原始碼**,每一種都必須被抓到。
     */
    @Test
    fun `覆核者實測過的四種拆法,每一種都必須被抓到`() {
        val home = read("AppScreen.kt")
        val onboarding = read("Onboarding.kt")
        // 前提:沒拆之前是乾淨的。否則下面的紅是本來就紅,證明不了任何事。
        assertEquals("拆之前首頁就已經是紅的,這個反向測試不算數", emptyList<String>(), homeProblems(home))
        assertEquals("拆之前引導頁就已經是紅的,這個反向測試不算數", emptyList<String>(), onboardingProblems(onboarding))

        val sabotages = listOf(
            Sabotage(
                "(a) 首頁失敗態那顆按鈕改成什麼都不做",
                home,
                home.replaceFirst("else -> onRefreshWords()", "else -> Unit"),
                ::homeProblems,
            ),
            Sabotage(
                "(b) 引導頁失敗屏那顆按鈕的 onClick 改成空的",
                onboarding,
                onboarding.replaceFirst("onClick = onRefreshWords,", "onClick = {},"),
                ::onboardingProblems,
            ),
            Sabotage(
                "(c) 首頁把整段畫 initError 的 if 區塊刪光",
                home,
                deleteBlockAt(home, "if (stage == SetupStage.FAILED && !initError.isNullOrBlank())"),
                ::homeProblems,
            ),
            Sabotage(
                "(d) 引導頁的 FAILED 那一格改回畫 PreparingBody(定義留著)",
                onboarding,
                onboarding.replaceFirst(FAILED_BODY_CALL, "PreparingBody(phase = phase)"),
                ::onboardingProblems,
            ),
        )

        for (s in sabotages) {
            // 植入本身要真的生效。錨點對不上的話,樹是沒改過的,
            // 而「規則沒叫」會被誤讀成「規則沒用」。
            assertNotEquals("${s.what}:這個拆法根本沒有植入成功,這一條反向測試等於沒做", s.original, s.mutated)
            val problems = s.rules(s.mutated)
            assertTrue(
                "${s.what}:拆掉之後**沒有任何一條規則叫** —— 那上面那些綠燈不代表任何事",
                problems.isNotEmpty(),
            )
        }
    }

    /** 上一版真正的寫法,也要被抓到。 */
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
        // 一條都沒接 = 每一條規則都要叫。用空字串當基準,免得日後加規則忘了改數字。
        assertTrue("首頁一條規則都沒有", homeProblems("").isNotEmpty())
        assertEquals(
            "上一版的寫法沒有被完整抓到 —— 那上面那條全綠不代表任何事",
            homeProblems("").size,
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

    /** 反向測試的另一半:**接對了不可以叫**。會亂叫的檢查一樣會被關掉。 */
    @Test
    fun `接對了不算問題`() {
        val good = """
            NotReadyStrip(
                stage = stage,
                initError = initError,
                onRefreshWords = onRefreshWords,
            )
            if (stage == SetupStage.FAILED && !initError.isNullOrBlank()) {
                Text(text = initError)
            }
            val action = actionOf(stage, failure)
            NotReadyAction.SWITCH_IME -> R.string.not_ready_switch
            else -> R.string.not_ready_refresh_words
            else -> onRefreshWords()
        """.trimIndent()
        assertEquals(emptyList<String>(), homeProblems(good))

        val goodOnboarding = """
            SetupStage.FAILED ->
                FailedBody(
                    error = initError,
                    onRefreshWords = onRefreshWords,
                )
            SetupStage.READY -> ReadyBody(onFinished = onFinished)
            private fun PreparingBody(phase: RimeRuntime.Phase) { }
            Button(onClick = onRefreshWords) { }
        """.trimIndent()
        assertEquals(emptyList<String>(), onboardingProblems(goodOnboarding))
    }

    /* ─────────────── 規則本體 ─────────────── */

    /**
     * 每一條都是**呼叫位置或資料流**。刻意不用 `contains("initError")` 這種
     * 整檔比對:那個字在參數宣告裡就有,永遠命中。
     */
    private fun homeProblems(src: String): List<String> = buildList {
        if (!PASSES_INIT_ERROR.containsMatchIn(src)) {
            add(
                "沒有人把 initError 傳給那條橫幅(找不到 `initError = initError`)—— " +
                    "失敗態帶不出原因"
            )
        }
        if (!PRINTS_INIT_ERROR.containsMatchIn(src)) {
            add(
                "橫幅收到了 initError 卻沒有畫出來(找不到 `text = initError`)—— " +
                    "使用者看到「準備失敗」但看不到任何原因,而問題回報就靠那一行"
            )
        }
        if (!GUARDS_INIT_ERROR.containsMatchIn(src)) {
            add(
                "那一行原因沒有只在 FAILED 且非空時才畫(找不到 " +
                    "`stage == SetupStage.FAILED && !initError.isNullOrBlank()`)"
            )
        }
        if (!ASKS_ACTION_OF.containsMatchIn(src)) {
            add(
                "首頁沒有呼叫 actionOf() —— 該不該給按鈕又變成寫在 Composable 裡的 if 了"
            )
        }
        if (!CLICK_CALLS_REFRESH.containsMatchIn(src)) {
            add(
                "失敗態那顆按鈕按下去什麼都不做(找不到 `else -> onRefreshWords()`)—— " +
                    "畫得出來但沒有接線,而畫面上看不出差別"
            )
        }
        if (!LABEL_REFRESH.containsMatchIn(src)) {
            add("失敗態那顆按鈕沒有文案(找不到 `else -> R.string.not_ready_refresh_words`)")
        }
    }

    private fun onboardingProblems(src: String): List<String> = buildList {
        if (ZERO_ARG_PREPARING.containsMatchIn(src)) {
            add(
                "PreparingBody 沒有參數 —— Compose 會跳過它的重組," +
                    "裡面讀的 @Volatile 欄位(phase / initError)一輩子畫不出來"
            )
        }
        val arm = failedArm(src)
        if (arm == null) {
            add("引導頁的 when(stage) 沒有 `SetupStage.FAILED ->` 這一屏,失敗會被畫成準備中")
        } else {
            // ⚠ 只在**那一格的範圍內**找。整檔 contains 會被三百行外的
            //   `private fun FailedBody(` 餵飽 —— 那正是上一版漏掉 (d) 的原因。
            if (!arm.contains("FailedBody(")) {
                add(
                    "FAILED 這一格畫的不是 FailedBody(...) —— 使用者會被關在引導頁裡" +
                        "(引導頁只有 ReadyBody 那條路會呼叫 onFinished)"
                )
            }
            if (!PASSES_ERROR_ARG.containsMatchIn(arm)) {
                add("FailedBody 沒有拿到 `error = initError` —— 那一屏說不出失敗原因")
            }
            if (!PASSES_REFRESH_ARG.containsMatchIn(arm)) {
                add("FailedBody 沒有拿到 `onRefreshWords = onRefreshWords` —— 那一屏沒有出路")
            }
        }
        if (!ON_CLICK_REFRESH.containsMatchIn(src)) {
            add(
                "失敗屏那顆按鈕的 onClick 不是 onRefreshWords(找不到 `onClick = onRefreshWords`)" +
                    " —— 按鈕畫得出來,按下去什麼都不會發生"
            )
        }
    }

    /**
     * `SetupStage.FAILED ->` 那一格的內容(到下一格為止)。
     * 找不到那一格時回 null。
     */
    private fun failedArm(src: String): String? {
        val head = FAILED_ARM.find(src) ?: return null
        val rest = src.substring(head.range.last + 1)
        val next = NEXT_ARM.find(rest)?.range?.first ?: rest.length
        return rest.substring(0, next)
    }

    /** 從 [marker] 開始,連同它後面那個大括號區塊一起刪掉(重現「整段刪光」)。 */
    private fun deleteBlockAt(src: String, marker: String): String {
        val i = src.indexOf(marker)
        require(i >= 0) { "找不到錨點:$marker" }
        val open = src.indexOf('{', i)
        require(open >= 0) { "錨點後面沒有大括號:$marker" }
        var depth = 0
        var j = open
        while (j < src.length) {
            if (src[j] == '{') depth++
            if (src[j] == '}') {
                depth--
                if (depth == 0) break
            }
            j++
        }
        require(depth == 0) { "大括號沒有配對:$marker" }
        return src.removeRange(i, j + 1)
    }

    private data class Sabotage(
        val what: String,
        val original: String,
        val mutated: String,
        val rules: (String) -> List<String>,
    )

    private fun read(name: String): String {
        val f = File(homeRoot, name)
        assertTrue("找不到 ${f.path}", f.isFile)
        return f.readText()
    }

    companion object {
        /** 單元測試的工作目錄是模組目錄(`android/app`)。 */
        private val homeRoot = File("src/main/java/org/luminakey/ime/home")

        /** 兩支檔案都是好幾千字元;讀到比這個少一定是路徑錯了。 */
        private const val MIN_CHARS = 3000

        private val ZERO_ARG_PREPARING = Regex("""fun PreparingBody\(\s*\)""")

        // ── 首頁:六條接線 ──
        private val PASSES_INIT_ERROR = Regex("""initError\s*=\s*initError\b""")
        private val PRINTS_INIT_ERROR = Regex("""text\s*=\s*initError\b""")
        private val GUARDS_INIT_ERROR =
            Regex("""stage\s*==\s*SetupStage\.FAILED\s*&&\s*!initError\.isNullOrBlank\(\)""")
        // ⚠ 併版時改過:實作端是 actionOf(stage, failure) —— 兩個引數那一版比
        //   單引數那一版嚴(它把「哪一種失敗」也納入「該不該給按鈕」),所以留
        //   實作、改判準。第二個引數**必須是變數 failure**,寫死成某一種失敗
        //   (覆核者的 P2)就不算數。
        private val ASKS_ACTION_OF =
            Regex("""=\s*actionOf\(\s*stage\s*,\s*failure\s*\)""")
        private val CLICK_CALLS_REFRESH = Regex("""else\s*->\s*onRefreshWords\(\)""")
        private val LABEL_REFRESH = Regex("""else\s*->\s*R\.string\.not_ready_refresh_words""")

        // ── 引導頁 ──
        private val FAILED_ARM = Regex("""SetupStage\.FAILED\s*->""")
        private val NEXT_ARM = Regex("""SetupStage\.[A-Z_]+\s*->""")
        private val PASSES_ERROR_ARG = Regex("""error\s*=\s*initError\b""")
        private val PASSES_REFRESH_ARG = Regex("""onRefreshWords\s*=\s*onRefreshWords\b""")
        private val ON_CLICK_REFRESH = Regex("""onClick\s*=\s*onRefreshWords\b""")

        /**
         * 拆法 (d) 用的原文。刻意連引數一起換掉:覆核者那一版是**編得起來的**
         * (PreparingBody 只收 phase),而編不起來的拆法證明不了守門有沒有牙齒。
         */
        private val FAILED_BODY_CALL = """
            |                FailedBody(
            |                    error = initError,
            |                    failure = failure,
            |                    onRefreshWords = onRefreshWords,
            |                    onFinished = onFinished,
            |                )
        """.trimMargin()
    }
}
