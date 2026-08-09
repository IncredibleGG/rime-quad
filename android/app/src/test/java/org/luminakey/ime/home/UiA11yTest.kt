package org.luminakey.ime.home

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * App 畫面上每一個摸得到的東西，TalkBack 都念得出它是什麼、會做什麼
 * （`docs/ui-design.md` 檢核表 F6）。
 *
 * ── 為什麼是這個形狀，而不是「檢查有沒有 contentDescription」────────────
 * 補 `contentDescription` 是最直覺、也最容易只做一半的做法。做一半的結果不是
 * 「無障礙做得不夠好」，是一顆**念得出名字、聚焦得到、輕點兩下卻不知道會發生
 * 什麼**的控制項。`keyboard/KeyA11yTest` 已經在鍵盤那一側踩過同一個坑。
 *
 * 所以這裡守的是**接線規則**，三條，全部是機械的：
 *
 *   1. `.clickable(` 一定要給 `onClickLabel` —— 否則 TalkBack 念的是
 *      「輕點兩下以**啟動**」，一句等於沒說的話。給了才會念
 *      「輕點兩下以**打開外觀**」。
 *   2. `.selectable(` / `.toggleable(` 一定要給 `role` —— 否則念不出
 *      「單選按鈕，已選取」或「開關，關閉」，使用者不知道自己在操作什麼、
 *      也不知道現在是哪個狀態。
 *   3. 一組 `.selectable(` 外面要有 `selectableGroup()` —— 少了它，
 *      TalkBack 念得出每一格的名字，卻念不出「第 2 個，共 3 個」，
 *      使用者不知道自己聽完了沒有。
 *
 * ── ⚠ 它抓不到什麼（誠實說明）──────────────────────────────────────────
 * 這裡驗的是**規則與接線**，跟 `KeyA11yTest` 一樣。它**不會**替你按一次：
 *   · `onClickLabel` 給的字**通順不通順**，只有人聽得出來；
 *   · TalkBack 的焦點順序合不合理、有沒有把整列併成一個節點，這裡看不到；
 *   · 螢幕上真的長出來的觸控目標有沒有 48dp（這裡只看得到宣告）；
 *   · Material 自己的元件（`Button` / `Switch` / `AlertDialog`）本來就帶語意，
 *     所以它們不在這三條規則的範圍內 —— 但**它們的文字寫得好不好，一樣只有人看得出來**。
 * 那一層寫在報告的「只有人做得到」清單裡，不假裝這裡做掉了。
 */
class UiA11yTest {

    /* ─────────────── 1. 真的掃一次 ─────────────── */

    @Test
    fun `每一個可點的東西都說得出點下去會怎樣`() {
        val files = scanScope()

        // G2：範圍非空。
        val names = files.map { it.name }.toSet()
        val missing = REQUIRED_FILES.filterNot { it in names }
        assertTrue("掃描範圍漏了：$missing（範圍寫錯必須是紅）", missing.isEmpty())

        val sites = files.flatMap { f -> findSites(f.readText()).map { f.name to it } }
        // 範圍非空的第二層：真的有找到互動點。全部改寫成別的寫法時要被發現。
        assertTrue(
            "一個互動點都沒掃到（下界 $MIN_SITES）—— 這代表寫法變了而規則沒跟上，" +
                "不代表畫面上沒有可點的東西",
            sites.size >= MIN_SITES,
        )

        val bad = sites.filter { !it.second.ok }
        assertTrue(
            buildString {
                appendLine("這幾個互動點對 TalkBack 是沉默的：")
                bad.forEach { (file, s) -> appendLine("  $file:${s.line}  ${s.why}\n      ${s.text}") }
            },
            bad.isEmpty(),
        )
    }

    @Test
    fun `一組單選外面有 selectableGroup`() {
        val offenders = scanScope().filter { f ->
            val src = f.readText()
            src.contains(".selectable(") && !src.contains("selectableGroup()")
        }.map { it.name }
        assertEquals(
            "這幾個檔案有 selectable 但沒有 selectableGroup —— TalkBack 會念不出" +
                "「第幾個，共幾個」，使用者不知道自己聽完了沒有：$offenders",
            emptyList<String>(),
            offenders,
        )
    }

    /* ─────────────── 2. 反向測試（G1）─────────────── */

    @Test
    fun `少了 onClickLabel 的 clickable 會被抓到`() {
        val src = """
            Row(Modifier.clickable(onClick = onClick)) { Text(title) }
        """.trimIndent()
        val bad = findSites(src).filter { !it.ok }
        assertEquals(1, bad.size)
        assertTrue(bad.single().why.contains("onClickLabel"))
    }

    @Test
    fun `少了 role 的 selectable 與 toggleable 會被抓到`() {
        val src = """
            Box(Modifier.selectable(selected = on, onClick = { pick() }))
            Row(Modifier.toggleable(value = v, onValueChange = set))
        """.trimIndent()
        assertEquals(
            listOf("role", "role"),
            findSites(src).filter { !it.ok }.map { if (it.why.contains("role")) "role" else it.why },
        )
    }

    /** 反向測試的另一半：**寫對了不可以叫**。 */
    @Test
    fun `寫對的接線不算違規`() {
        val src = """
            Row(Modifier.clickable(onClickLabel = openLabel, onClick = onClick))
            Box(
                Modifier.selectable(
                    selected = on,
                    role = Role.RadioButton,
                    onClick = { onSelect(value) },
                )
            )
            Row(
                Modifier.toggleable(
                    value = checked,
                    role = Role.Switch,
                    onValueChange = onCheckedChange,
                )
            )
        """.trimIndent()
        assertEquals(emptyList<String>(), findSites(src).filter { !it.ok }.map { it.why })
    }

    /* ─────────────── 掃描器本體 ─────────────── */

    private class Site(val line: Int, val ok: Boolean, val why: String, val text: String)

    /**
     * 找出互動修飾詞，並看它後面那一段參數裡有沒有必要的鍵。
     *
     * 「後面那一段」= 從修飾詞開始的括號配對到結束。用括號配對而不是
     * 「同一行」，因為這幾個呼叫幾乎都跨好幾行。
     */
    private fun findSites(source: String): List<Site> {
        val out = ArrayList<Site>()
        for (m in CALL.findAll(source)) {
            val name = m.groupValues[1]
            val open = source.indexOf('(', m.range.first)
            if (open < 0) continue
            val args = balancedArgs(source, open) ?: continue
            val line = source.take(m.range.first).count { it == '\n' } + 1
            val required = when (name) {
                "clickable" -> "onClickLabel"
                else -> "role"
            }
            val ok = args.contains("$required =") || args.contains("$required=")
            out += Site(
                line = line,
                ok = ok,
                why = "`$name` 沒有給 `$required`",
                text = args.lineSequence().first().trim().take(80),
            )
        }
        return out
    }

    /** 從 `open` 這個左括號開始，回傳到配對右括號為止的內容。 */
    private fun balancedArgs(s: String, open: Int): String? {
        var depth = 0
        for (i in open until s.length) {
            when (s[i]) {
                '(' -> depth++
                ')' -> {
                    depth--
                    if (depth == 0) return s.substring(open + 1, i)
                }
            }
        }
        return null
    }

    private fun scanScope(): List<File> =
        (homeRoot.listFiles().orEmpty().filter { it.name.endsWith(".kt") } +
            listOfNotNull(storeScreen.takeIf { it.isFile }))
            .sortedBy { it.name }

    companion object {
        private val homeRoot = File("src/main/java/org/luminakey/ime/home")
        private val storeScreen = File("src/main/java/org/luminakey/ime/store/StoreScreen.kt")

        private val CALL = Regex("""\.(clickable|selectable|toggleable)\s*\(""")

        /**
         * ⚠ G4：「還有誰不在範圍內」——
         * 與 [DesignTokenTest] 同一個範圍，理由也一樣。`prefs/KeyRemapSection.kt`
         * 是規範點名「離規範最遠」的一頁（§7.7），它**不在**這裡，
         * 而且它現在就有沒帶 `onClickLabel` 的互動點。不是通過了，是沒被看。
         */
        private val REQUIRED_FILES = listOf(
            "AppScreen.kt", "KeyboardChoice.kt", "Onboarding.kt",
            "SettingsPages.kt", "Ui.kt", "StoreScreen.kt",
        )

        /** 2026-08-09 實測 8 個。取 6 當下界，留一點正常增減的空間。 */
        private const val MIN_SITES = 6
    }
}
