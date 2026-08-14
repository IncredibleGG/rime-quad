package org.luminakey.ime.home

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

/**
 * 工單 #106:退掉第二道系統對話框 → 回到 app,畫面一個字都沒變。
 *
 * （註:本檔刻意不在註解與訊息裡寫「數字＋秒」——`DeployEstimateTest` 會把
 * 「數字＋秒」一律當成有人又抄了一份「整理字詞要多久」的估計值。）
 *
 * ① 判準;② 引導頁真的接上了它;③ 預告那句話三種語言都在。
 */
class SetupNudgeTest {

    /* ─────────────── ① 判準 ─────────────── */

    /** 走查踩到的那一條:去過系統設定,回來仍然不在啟用清單裡。 */
    @Test
    fun `去過系統設定而仍然沒啟用要說話`() {
        assertEquals(
            SetupNudge.STEP_1_DID_NOT_TAKE,
            nudgeOf(SetupStage.NOT_ENABLED, triedStep1 = true, triedStep2 = false),
        )
    }

    /** 還沒去過就不要多嘴 —— 第一次看到這個畫面的人沒有做錯任何事。 */
    @Test
    fun `還沒去過就什麼都不說`() {
        assertEquals(
            SetupNudge.NONE,
            nudgeOf(SetupStage.NOT_ENABLED, triedStep1 = false, triedStep2 = false),
        )
    }

    @Test
    fun `第二步同一個形狀`() {
        assertEquals(
            SetupNudge.STEP_2_DID_NOT_TAKE,
            nudgeOf(SetupStage.ENABLED_NOT_DEFAULT, triedStep1 = true, triedStep2 = true),
        )
        assertEquals(
            "叫過選擇器之前不該說「還沒切過來」",
            SetupNudge.NONE,
            nudgeOf(SetupStage.ENABLED_NOT_DEFAULT, triedStep1 = true, triedStep2 = false),
        )
    }

    /**
     * 成功之後**一句都不准留**。
     *
     * 這一條擋的是最尷尬的失效:使用者第二趟成功了,畫面上卻還掛著
     * 「剛才那一趟沒有成功」。
     */
    @Test
    fun `事情成立之後不再說話`() {
        for (stage in listOf(SetupStage.PREPARING, SetupStage.READY, SetupStage.FAILED)) {
            assertEquals(
                "$stage 時不該再提醒",
                SetupNudge.NONE,
                nudgeOf(stage, triedStep1 = true, triedStep2 = true),
            )
        }
    }

    /**
     * 第 1 步沒成立的時候不要講第 2 步的事 —— 順序與 [stageOf] 一致。
     */
    @Test
    fun `前置條件沒成立就先講前置條件`() {
        assertEquals(
            SetupNudge.STEP_1_DID_NOT_TAKE,
            nudgeOf(SetupStage.NOT_ENABLED, triedStep1 = true, triedStep2 = true),
        )
    }

    @Test
    fun `每一種提醒都有一句話，NONE 沒有`() {
        assertNull(nudgeBodyRes(SetupNudge.NONE))
        for (n in SetupNudge.entries.filter { it != SetupNudge.NONE }) {
            assertNotNull("$n 沒有對應的字串 —— 那它在畫面上就是不存在的", nudgeBodyRes(n))
        }
    }

    /* ─────────────── ② 接線 ─────────────── */

    private fun onboarding(): String {
        val f = File("src/main/java/org/luminakey/ime/home/Onboarding.kt")
        assertTrue("找不到 ${f.path}", f.isFile)
        return f.readText().lineSequence().joinToString("\n") { line ->
            val i = line.indexOf("//")
            if (i >= 0 && line.take(i).isBlank()) "" else line
        }
    }

    /**
     * 兩顆按鈕都要**記下自己被按過**,否則判準永遠拿到 false,
     * 而「永遠不提醒」與「沒接線」在畫面上一模一樣。
     */
    @Test
    fun `兩顆按鈕都記下了自己被按過`() {
        val src = onboarding()
        assertTrue(
            "第 1 步那顆按鈕沒有記下 triedStep1 —— 提醒永遠不會出現",
            Regex("""triedStep1 = true\s*\n\s*openImeSettings\(context\)""").containsMatchIn(src),
        )
        assertTrue(
            "第 2 步那顆按鈕沒有記下 triedStep2",
            Regex("""triedStep2 = true\s*\n\s*showImePicker\(context\)""").containsMatchIn(src),
        )
    }

    /** 判準的結果真的被畫出來了(接到 StepRow 的 `alert`)。 */
    @Test
    fun `判準的結果真的接到畫面上`() {
        val src = onboarding()
        assertTrue("引導頁沒有呼叫 nudgeOf", src.contains("nudgeOf(stage, triedStep1, triedStep2)"))
        assertTrue(
            "算出來了卻沒有畫 —— 這正是這條工單抓到的形狀",
            src.contains("alert = nudgeBodyRes(nudge)"),
        )
        assertTrue("StepRow 沒有 alert 參數", src.contains("alert: String? = null"))
    }

    /** 預告那一句在**按下去之前**就在畫面上。 */
    @Test
    fun `預告那一句有被畫出來`() {
        assertTrue(
            "引導頁沒有預告 Android 會問什麼 —— 使用者最可能放棄的那一刻沒有人說話",
            onboarding().contains("R.string.step_1_heads_up"),
        )
    }

    /* ─────────────── ③ 三種語言 ─────────────── */

    @Test
    fun `三種語言都有這三句`() {
        val ids = listOf("step_1_heads_up", "step_1_did_not_take", "step_2_did_not_take")
        val blanks = mutableListOf<String>()
        for (dir in listOf("values", "values-b+zh+Hant", "values-b+zh+Hans")) {
            val table = load(dir)
            for (id in ids) {
                if (table[id].isNullOrBlank()) blanks += "$dir/$id"
            }
        }
        assertEquals("少一份的下場是那一句在該語言下靜靜地變成英文或空白:$blanks", emptyList<String>(), blanks)
    }

    /**
     * 預告那一句必須真的講到「兩次」這件事 —— 那才是靜默失敗的成因。
     * 一句泛泛的「系統會問你一些問題」等於沒有預告。
     */
    @Test
    fun `預告講到了會問兩次`() {
        val en = load("values")["step_1_heads_up"].orEmpty()
        assertTrue("英文版沒有講到「兩個問題」:$en", en.contains("two questions", ignoreCase = true))
        assertTrue("英文版沒有講到取消第二題的後果:$en", en.contains("second", ignoreCase = true))
        val hant = load("values-b+zh+Hant")["step_1_heads_up"].orEmpty()
        assertTrue("繁體版沒有講到「兩個問題」:$hant", hant.contains("兩個問題"))
        assertTrue("繁體版沒有講到第二題:$hant", hant.contains("第二題"))
    }

    private fun load(dir: String): Map<String, String> {
        val f = File("src/main/res/$dir/strings.xml")
        assertTrue("找不到 ${f.path}", f.isFile)
        val doc = DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(f)
        val out = LinkedHashMap<String, String>()
        val nodes = doc.getElementsByTagName("string")
        for (i in 0 until nodes.length) {
            val e = nodes.item(i)
            val name = e.attributes?.getNamedItem("name")?.nodeValue ?: continue
            out[name] = e.textContent.orEmpty()
        }
        return out
    }
}
