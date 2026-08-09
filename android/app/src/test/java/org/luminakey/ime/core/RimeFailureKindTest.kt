package org.luminakey.ime.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * **每一條走到 `Phase.FAILED` 的路都要說出自己是哪一種失敗。**
 *
 * ── 為什麼這件事需要守 ──────────────────────────────────────────────────
 * `RimeRuntime` 有五條路會走到 FAILED，而畫面上那顆「重新整理字詞」只有
 * 其中一條（部署失敗）按得動 —— 另外四條 `RimeCore.isInitialized` 是 false，
 * `DeployGate` 第一行就回頭。要在那四條路上不畫按鈕，前提是**知道現在是哪一條**。
 *
 * 所以 `RimeRuntime.failure` 必須在每一條失敗路徑上都被設定。忘掉一條的後果
 * 不是崩潰，是**那條路上又出現一顆按不動的按鈕**，而且沒有任何執行期徵兆。
 *
 * ── 為什麼用結構驗，而不是 `grep -q Failure.UNPACK` ─────────────────────
 * 因為 `grep` 掃整支檔案：`Failure.UNPACK` 只要在列舉宣告裡出現過一次
 * （它一定會），那條檢查就永遠是綠的，即使沒有任何人呼叫它。
 *
 * 這裡驗的是**呼叫位置**：
 *   1. 整支檔案裡 `setPhase(Phase.FAILED)` 只有一處，而且就在 `private fun fail(`
 *      的函式體內 —— 也就是說沒有人繞過 `fail()` 直接進 FAILED；
 *   2. `fail(` 的每一個呼叫點第一個參數都是 `Failure.<某一種>`；
 *   3. 這些呼叫點用到的種類**恰好**是 `Failure` 除了 `NONE` 以外的全部 ——
 *      少一種代表有路沒標，多一種代表列舉裡有沒人用的值。
 *   4. 每一處 `initError = null`（成功）旁邊都要有 `failure = Failure.NONE`，
 *      否則上一次失敗的種類會留到下一次，畫面會照著舊種類決定要不要給按鈕。
 *
 * ── 它抓不到什麼（誠實說明）────────────────────────────────────────────
 * 這是**原始碼文字比對**，不是跑一次 `RimeRuntime`（那需要 Android 的
 * `Handler` 與真的 `.so`，JVM 單元測試裡辦不到）。它證明得了「每條失敗路徑
 * 都經過 `fail()` 且帶了種類」，證明不了「那個種類選對了」—— 例如把 ABI 不符
 * 標成 `UNPACK`，這裡不會叫。那一層要人看。
 */
class RimeFailureKindTest {

    /* ─────────────── 1. 真的掃一次 ─────────────── */

    @Test
    fun `進入 FAILED 的唯一入口是 fail()`() {
        val src = read()
        assertEquals(
            "setPhase(Phase.FAILED) 出現了 ${countFailedTransitions(src)} 次。" +
                "它必須只有一處，而且在 fail() 裡 —— 多一處就是一條沒有標種類的失敗路徑",
            1,
            countFailedTransitions(src),
        )
        assertTrue(
            "setPhase(Phase.FAILED) 不在 fail() 的函式體內 —— 有人繞過去了",
            failedTransitionIsInsideFail(src),
        )
    }

    @Test
    fun `每一條失敗路徑都標了種類`() {
        val src = read()
        // G2：範圍非空。路徑寫錯或函式被改名時必須是紅，不是零個問題。
        assertTrue("RimeRuntime.kt 只讀到 ${src.length} 個字元，路徑大概錯了", src.length >= 8000)
        assertTrue(
            "RimeRuntime.kt 裡找不到 `private fun fail(` —— 這條測試已經對不上實作了",
            src.contains(FAIL_DECL),
        )

        val used = kindsPassedToFail(src)
        val declared = declaredKinds(src) - "NONE"
        assertEquals(
            "fail() 的呼叫點用到的種類與宣告的不一致。" +
                "少一種＝有失敗路徑沒標；多一種＝列舉裡有沒人用的值",
            declared.sorted(),
            used.sorted(),
        )
        assertTrue("fail() 至少要有五個呼叫點，現在只有 ${used.size} 種", used.size >= 5)
    }

    @Test
    fun `成功時把失敗種類清掉`() {
        val src = read()
        val lines = src.lines()
        val clears = lines.indices.filter { lines[it].trim() == "initError = null" }
        assertTrue("找不到任何 `initError = null` —— 這條測試對不上實作了", clears.isNotEmpty())
        val unpaired = clears.filterNot { i ->
            lines.getOrNull(i + 1)?.trim() == "failure = Failure.NONE"
        }
        assertEquals(
            "這幾行把 initError 清掉了卻沒有把 failure 清掉（行號從 1 起算）：" +
                "${unpaired.map { it + 1 }}。" +
                "留著舊種類的後果是下一次成功之後、再一次失敗時畫面照舊種類給按鈕",
            emptyList<Int>(),
            unpaired,
        )
    }

    /* ─────────────── 2. 反向測試（G1）─────────────── */

    /** 餵上一版真正的寫法：直接 `initError = …` 然後 `setPhase(Phase.FAILED)`。 */
    @Test
    fun `上一版的寫法會被抓到`() {
        val old = """
            if (!RimeCore.libraryLoaded) {
                initError = "librime_jni.so failed to load"
                setPhase(Phase.FAILED)
                return
            }
            if (!RimeCore.abiCompatible()) {
                initError = "ABI mismatch"
                setPhase(Phase.FAILED)
                return
            }
        """.trimIndent()
        assertEquals("兩處直接轉 FAILED 沒有被算到", 2, countFailedTransitions(old))
        assertTrue("沒有 fail() 卻說在裡面", !failedTransitionIsInsideFail(old))
        assertEquals("這一版一個種類都沒標", emptyList<String>(), kindsPassedToFail(old))
    }

    /** 反向測試的另一半：**寫對了不可以叫**。 */
    @Test
    fun `寫對的形狀不算問題`() {
        val good = """
            private fun fail(kind: Failure, message: String) {
                initError = message
                failure = kind
                setPhase(Phase.FAILED)
            }
            fun a() { fail(Failure.UNPACK, "x") }
            fun b() {
                fail(
                    Failure.ABI_MISMATCH,
                    "y",
                )
            }
        """.trimIndent()
        assertEquals(1, countFailedTransitions(good))
        assertTrue(failedTransitionIsInsideFail(good))
        assertEquals(listOf("ABI_MISMATCH", "UNPACK"), kindsPassedToFail(good).sorted())
    }

    /* ─────────────── 掃描器本體 ─────────────── */

    private fun countFailedTransitions(src: String): Int =
        FAILED_TRANSITION.findAll(strip(src)).count()

    private fun failedTransitionIsInsideFail(src: String): Boolean {
        val clean = strip(src)
        val at = clean.indexOf(FAIL_DECL)
        if (at < 0) return false
        val body = bodyRangeAt(clean, at) ?: return false
        return FAILED_TRANSITION.findAll(clean).all { it.range.first in body }
    }

    /** `fail(` 的呼叫點（不含宣告）第一個參數是哪一個 `Failure.` 常數。 */
    private fun kindsPassedToFail(src: String): List<String> =
        FAIL_CALL.findAll(strip(src)).map { it.groupValues[1] }.toList()

    private fun declaredKinds(src: String): List<String> {
        val clean = strip(src)
        val at = clean.indexOf("enum class Failure")
        assertTrue("RimeRuntime.kt 裡找不到 `enum class Failure`", at >= 0)
        val body = bodyRangeAt(clean, at) ?: error("Failure 的大括號沒有配對")
        return ENUM_CONST.findAll(clean.substring(body))
            .map { it.groupValues[1] }
            .distinct()
            .toList()
    }

    /** 從 [at] 之後的第一個 `{` 起，括號配對到結束；回傳大括號**內部**的範圍。 */
    private fun bodyRangeAt(src: String, at: Int): IntRange? {
        val open = src.indexOf('{', at)
        if (open < 0) return null
        var depth = 0
        for (i in open until src.length) {
            when (src[i]) {
                '{' -> depth++
                '}' -> {
                    depth--
                    if (depth == 0) return (open + 1) until i
                }
            }
        }
        return null
    }

    /**
     * 砍掉整行註解與區塊註解。
     *
     * ⚠ 非做不可：本檔的 KDoc 裡就寫著 `setPhase(Phase.FAILED)` 這串字
     * （解釋為什麼只能有一處）。不砍註解的話，「只有一處」永遠是假的。
     */
    private fun strip(src: String): String =
        src.lineSequence().joinToString("\n") { line ->
            val i = line.indexOf("//")
            if (i >= 0 && line.take(i).isBlank()) "" else line
        }.replace(BLOCK_COMMENT, "")

    private fun read(): String {
        val f = File("src/main/java/org/luminakey/ime/core/RimeRuntime.kt")
        assertTrue("找不到 ${f.path}", f.isFile)
        return f.readText()
    }

    companion object {
        private const val FAIL_DECL = "private fun fail("

        private val FAILED_TRANSITION = Regex("""setPhase\(Phase\.FAILED\)""")

        /** `fail(Failure.X` —— 允許換行與空白，但第一個參數必須是種類。 */
        private val FAIL_CALL = Regex("""(?<!fun )\bfail\(\s*Failure\.([A-Z_]+)""")

        /** 列舉常數：行首縮排 + 全大寫 + 逗號。 */
        private val ENUM_CONST = Regex("""(?m)^\s{8}([A-Z][A-Z_]*),""")

        private val BLOCK_COMMENT = Regex("""/\*[\s\S]*?\*/""")
    }
}
