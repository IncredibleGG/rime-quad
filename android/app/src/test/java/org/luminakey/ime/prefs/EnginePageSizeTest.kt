package org.luminakey.ime.prefs

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.theme.RepoFixtures
import java.io.File

/**
 * 「引擎一頁給幾個候選」只有一份真相,而那一份在
 * `core/data/shared/default.yaml` 的 `menu/page_size`。
 *
 * ── 為什麼需要這一條 ────────────────────────────────────────────────────
 * 這是一個真的發生過的事故,而且**兩邊各自都是對的**:
 *
 *   · 批 1 量到引擎一頁給 5 個,於是把設定頁的「一次顯示幾個」砍成 3/4/5,
 *     並寫下 `PrefLevels.ENGINE_PAGE_SIZE = 5` 這個常數。
 *   · 候選詞那條線同時把 `menu/page_size` 從 5 改成 9。
 *
 * 兩邊分開看都成立,合起來 `ENGINE_PAGE_SIZE = 5` 就是一句假話 —— 而當時
 * 守著檔位的那條測試**不會紅**(3/4/5 都 ≤ 9)。使用者拿到的是:預設仍然
 * 畫 9 個,設定列卻顯示成「5 個」,碰一下就永久鎖在 5、回不到 9。
 *
 * 一個寫死的常數沒有辦法知道另一個檔案改了。所以那個常數現在是
 * **建置期從這份 yaml 產生**的(見 android/app/build.gradle.kts 的
 * `generateEnginePageSize`),而這一條是它的反向確認:如果哪天產生器壞了、
 * 或有人手動把常數改回去,這裡會紅。
 *
 * ⚠ 這份 yaml 是 `scripts/collect_data.sh` 的產物,不進版控。
 *   它同時被宣告成 `test` 任務的輸入(見 build.gradle.kts 的
 *   `tasks.withType<Test>`),否則改了它這條測試會判 UP-TO-DATE 不跑 ——
 *   那正是「該紅的時候安靜地不跑」。
 */
class EnginePageSizeTest {

    private val defaultYaml: File
        get() = File(RepoFixtures.coreDir, "data/shared/default.yaml")

    /**
     * 從 `default.yaml` 讀出頂層 `menu:` 底下的 `page_size`。
     *
     * 刻意不用 `grep page_size` 那種讀法:同一份資料裡
     * `stroke.schema.yaml` 也有一個 `page_size`,而方案自己的那一個
     * 與 `default.yaml` 的預設不是同一件事。
     */
    private fun pageSizeFromData(): Int {
        val lines = defaultYaml.readLines()
        var inMenu = false
        for (line in lines) {
            if (line.isBlank() || line.trimStart().startsWith("#")) continue
            // 頂層鍵:第一個字元不是空白。
            if (!line[0].isWhitespace()) {
                inMenu = line.trimEnd().removeSuffix(":") == "menu"
                continue
            }
            if (!inMenu) continue
            val t = line.trim()
            if (t.startsWith("page_size:")) {
                return t.removePrefix("page_size:").trim().toInt()
            }
        }
        throw AssertionError(
            "${defaultYaml.absolutePath} 裡找不到頂層 menu/page_size。" +
                " 先跑 scripts/collect_data.sh;若上游改了鍵名,這條測試與" +
                " build.gradle.kts 的 generateEnginePageSize 要一起更新。"
        )
    }

    @Test
    fun `隨附資料存在`() {
        assertTrue(
            "找不到 ${defaultYaml.absolutePath} —— 先跑 scripts/collect_data.sh",
            defaultYaml.isFile,
        )
    }

    @Test
    fun `ENGINE_PAGE_SIZE 就是隨附資料裡的 menu-page_size`() {
        assertEquals(
            "PrefLevels.ENGINE_PAGE_SIZE 與 core/data/shared/default.yaml 的" +
                " menu/page_size 對不上。這個常數不可以是寫死的 —— 它由" +
                " build.gradle.kts 的 generateEnginePageSize 從那份 yaml 產生。",
            pageSizeFromData(),
            PrefLevels.ENGINE_PAGE_SIZE,
        )
    }

    /**
     * 檔位的上限**就是**引擎那一頁,不是「小於等於」。
     *
     * 只守「≤」的話,`page_size` 調大之後最後一檔會安靜地留在舊值 ——
     * 使用者選得到的最大值比引擎給得出來的少,而畫面上看不出任何異常。
     * 那就是這一輪修的那個形狀的鏡像。
     */
    @Test
    fun `最後一檔正好是引擎那一頁`() {
        val last = PrefLevels.withCandidateCount(
            UserPrefs(),
            PrefLevels.CANDIDATE_COUNT_LABELS.size - 1,
        ).candidateCount
        assertEquals(
            "最後一檔應該正好等於引擎一頁的數量。改了 menu/page_size 就要一起改" +
                " PrefLevels.CANDIDATE_COUNTS 與三份 strings.xml 的" +
                " levels_candidate_count。",
            PrefLevels.ENGINE_PAGE_SIZE,
            last,
        )
    }

    /**
     * **畫面上那個數字必須就是實際會畫出來的數量。**
     *
     * 檔位的值是程式碼裡的 `CANDIDATE_COUNTS`，標籤是三份 `strings.xml` 的
     * `levels_candidate_count`。`StringCatalogTest` 只釘住**項數**相同 ——
     * 於是「值改了、標籤沒改」是一條完全安靜的路:設定列上寫著「9 個」,
     * 按下去畫 11 個(或 7 個),而三份翻譯、項數、round-trip 全部是綠的。
     *
     * 那正是這一輪修的那個缺陷的形狀:**設定列上的數字對使用者說謊。**
     * 所以這裡逐項比對標籤裡的數字與該檔位真正寫進偏好的值。
     */
    @Test
    fun `三份語系的檔位標籤都寫著它真正會畫出來的數量`() {
        val values = PrefLevels.CANDIDATE_COUNT_LABELS.indices.map { i ->
            PrefLevels.withCandidateCount(UserPrefs(), i).candidateCount
        }
        val digits = Regex("\\d+")
        for (dir in listOf("values", "values-b+zh+Hant", "values-b+zh+Hans")) {
            val items = readArray(File("src/main/res/$dir/strings.xml"), "levels_candidate_count")
            assertEquals("$dir 的 levels_candidate_count 項數與檔位數不符", values.size, items.size)
            items.forEachIndexed { i, label ->
                val n = digits.find(label)?.value?.toIntOrNull()
                assertEquals(
                    "$dir 的第 ${i + 1} 個標籤是「$label」,但那一檔實際畫 ${values[i]} 個。" +
                        " 改了 menu/page_size 就要一起改 PrefLevels.CANDIDATE_COUNTS" +
                        " 與三份 strings.xml。",
                    values[i],
                    n,
                )
            }
        }
    }

    private fun readArray(f: File, name: String): List<String> {
        assertTrue("找不到 ${f.absolutePath}", f.isFile)
        val doc = javax.xml.parsers.DocumentBuilderFactory.newInstance()
            .newDocumentBuilder().parse(f)
        val arrays = doc.getElementsByTagName("string-array")
        for (i in 0 until arrays.length) {
            val e = arrays.item(i)
            if (e.attributes?.getNamedItem("name")?.nodeValue != name) continue
            val out = ArrayList<String>()
            val kids = e.childNodes
            for (k in 0 until kids.length) {
                if (kids.item(k).nodeName == "item") out += kids.item(k).textContent.orEmpty().trim()
            }
            return out
        }
        throw AssertionError("${f.absolutePath} 裡找不到 <string-array name=\"$name\">")
    }
}
