package org.luminakey.ime.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * **簡繁是一組互斥選項,而 librime 不替你維持互斥。**
 *
 * ── 這裡在守什麼 ────────────────────────────────────────────────────────
 * 使用者實機回報:「無論是簡體還是繁體 打出來候選詞都是一樣的 有繁體 有簡體」。
 *
 * 隨附的 luna_pinyin 家族用一組 radio 表達字形:
 *
 *     - options: [ zh_hant, zh_hans, zh_hant_hk, zh_hant_tw ]
 *       states:  [ 傳統漢字, 简化字,  香港字形,   臺灣字形 ]
 *
 * 而 `rs_set_option` **只是把那一格設成真** —— 維持 radio 互斥的是 librime 的
 * switcher,只有使用者從方案選單裡選的時候才會跑。程式自己設的話,同組可以
 * 同時有兩支為真,opencc 於是把 t2s 之後的結果再串一次 t2tw,候選列就簡繁混雜。
 *
 * 在 emulator-5558 上用 rime_console 對 luna_pinyin_tw 打 `guojia` 實測過:
 *
 *     只開 zh_hant_tw            → 國家 郭嘉 國際 國籍 過激
 *     zh_hant_tw + zh_hans 都開  → 国家 郭嘉 国际 国籍 过激
 *     只開 zh_hans               → 国家 郭嘉 国际 国籍 过激
 *
 * ── 為什麼是純函式,而不是在 applyVariant 裡驗 ──────────────────────────
 * 那一段原本埋在 `RimeInputMethodService` 裡,要驗它得先有 session、有引擎、
 * 有詞庫 —— 也就是只有真機跑得動,而真機不在 CI 上。抽成純函式之後,
 * 「同組恰好一支為真」這個**不變式**在 JVM 上就守得住。
 *
 * 四端共用同一個模型:Windows 的 `PlanVariant`(windows/common/schema_choice.h)
 * 已經是這個形狀,行為要一致 —— 序列長度固定、simplification 在最前、
 * 「先關再開」把互斥寫進**順序**裡,而不是靠「應該看不到中間態」。
 */
class VariantPlanTest {

    private val HANS = VariantPlan.OPT_ZH_HANS
    private val HANT = VariantPlan.OPT_ZH_HANT
    private val HK = VariantPlan.OPT_ZH_HANT_HK
    private val TW = VariantPlan.OPT_ZH_HANT_TW

    /** 套用計畫之後,同組裡為真的是哪幾支(照宣告順序)。 */
    private fun onesAfter(state: Set<String>, plan: VariantPlan.Plan): List<String> {
        val next = VariantPlan.applyTo(state, plan.assigns)
        return VariantPlan.VARIANT_OPTIONS.filter { it in next }
    }

    /** 起始狀態的完整清單:乾淨的、空的、以及使用者手上已經髒掉的。 */
    private val startStates: List<Set<String>> = listOf(
        setOf(TW),                 // 預設的 luna_pinyin_tw
        setOf(HK),
        setOf(HANT),
        setOf(HANS),
        emptySet(),                // 方案沒有 reset,luna_pinyin 就是這樣
        setOf(TW, HANS),           // ⚠ 舊版切過一次簡體之後的實際狀態
        setOf(HK, HANS),
        setOf(HANT, HANS),
        VariantPlan.VARIANT_OPTIONS.toSet(),  // 全開,最壞的情況
    )

    // ── 不變式:套用之後同組恰好一支為真 ────────────────────────────────

    @Test
    fun `任何起始狀態套用之後同組恰好一支為真`() {
        for (start in startStates) {
            for (want in listOf(true, false)) {
                for (remembered in listOf(null, TW, HK, HANT)) {
                    val plan = VariantPlan.plan(start, remembered, want)
                    val ones = onesAfter(start, plan)
                    assertEquals(
                        "起始=$start 要簡體=$want 記得=$remembered 之後應該恰好一支為真",
                        1, ones.size,
                    )
                }
            }
        }
    }

    @Test
    fun `要簡體就是zh_hans_要繁體就是繁體那三支之一`() {
        for (start in startStates) {
            for (remembered in listOf(null, TW, HK, HANT)) {
                val simp = VariantPlan.plan(start, remembered, true)
                assertEquals("起始=$start 記得=$remembered", listOf(HANS), onesAfter(start, simp))

                val trad = VariantPlan.plan(start, remembered, false)
                val ones = onesAfter(start, trad)
                assertTrue(
                    "起始=$start 記得=$remembered 要繁體卻得到 $ones",
                    ones.size == 1 && ones[0] in VariantPlan.TRADITIONAL_OPTIONS,
                )
            }
        }
    }

    // ── 往返:切一趟簡體再切回來,要回到**原本那一支** ────────────────────

    /**
     * 這是缺陷本身:舊版切成簡體之後就永遠回不到繁體。
     *
     * 舊版的狀態轉移(VARIANT_OPTIONS 的順序是 zh_hans 打頭):
     *   {zh_hant_tw} → 選簡 → current=zh_hant_tw,記住它,設 zh_hans=true
     *                       → {zh_hant_tw, zh_hans},輸出簡體
     *                → 選繁 → current=**zh_hans**(它排在最前面)→ 走 else-if,
     *                         設 savedVariantOption(zh_hant_tw)=true —— 那一支
     *                         本來就是真,而 zh_hans 從來沒被關掉 → 狀態不變。
     */
    @Test
    fun `臺灣字形切一趟簡體再切回來仍是臺灣字形`() {
        assertRoundTrip(setOf(TW), TW)
    }

    @Test
    fun `香港字形切一趟簡體再切回來仍是香港字形`() {
        assertRoundTrip(setOf(HK), HK)
    }

    @Test
    fun `傳統漢字切一趟簡體再切回來仍是傳統漢字`() {
        assertRoundTrip(setOf(HANT), HANT)
    }

    /**
     * 切回繁體要**還原原本那一支**,不是硬設 zh_hant:使用者原本在「臺灣字形」,
     * 切一趟簡體再回來卻變成「傳統漢字」,字形會整批改變,那不是他要求的。
     */
    private fun assertRoundTrip(start: Set<String>, expected: String) {
        var state = start
        var remembered: String? = null

        val toSimp = VariantPlan.plan(state, remembered, true)
        state = VariantPlan.applyTo(state, toSimp.assigns)
        remembered = toSimp.remembered
        assertEquals("切成簡體之後只該剩 zh_hans", listOf(HANS), VariantPlan.VARIANT_OPTIONS.filter { it in state })

        val back = VariantPlan.plan(state, remembered, false)
        state = VariantPlan.applyTo(state, back.assigns)
        remembered = back.remembered
        assertEquals(
            "切回繁體要還原成 $expected",
            listOf(expected), VariantPlan.VARIANT_OPTIONS.filter { it in state },
        )
    }

    /** 來回好幾趟也不能漂移 —— 記憶不能被自己弄髒。 */
    @Test
    fun `連續來回三趟仍然回到臺灣字形`() {
        var state: Set<String> = setOf(TW)
        var remembered: String? = null
        repeat(3) {
            val a = VariantPlan.plan(state, remembered, true)
            state = VariantPlan.applyTo(state, a.assigns); remembered = a.remembered
            val b = VariantPlan.plan(state, remembered, false)
            state = VariantPlan.applyTo(state, b.assigns); remembered = b.remembered
        }
        assertEquals(listOf(TW), VariantPlan.VARIANT_OPTIONS.filter { it in state })
    }

    /**
     * ⚠ 記住的那一支**不能是 zh_hans**。舊版用
     * `VARIANT_OPTIONS.firstOrNull { 開著 }` 算「原本那一支」,而 zh_hans 排在
     * 清單最前面 —— 一旦處在簡體狀態,算出來的就是 zh_hans 自己,記憶當場被弄髒。
     */
    @Test
    fun `記住的永遠是繁體那一支_不會被zh_hans弄髒`() {
        for (start in startStates) {
            for (want in listOf(true, false)) {
                val plan = VariantPlan.plan(start, TW, want)
                assertTrue(
                    "起始=$start 要簡體=$want 記成了 ${plan.remembered}",
                    plan.remembered != HANS,
                )
            }
        }
    }

    @Test
    fun `處在簡體狀態時不會忘記原本的繁體選擇`() {
        val plan = VariantPlan.plan(setOf(HANS), HK, true)
        assertEquals(HK, plan.remembered)
    }

    // ── 已經裝過舊版的人:狀態是髒的,要被修正 ──────────────────────────

    /**
     * **這一條是給既有使用者的。** 他手上的 session 現在就是 {zh_hant_tw, zh_hans}
     * 兩支同時為真 —— 升級之後第一次切簡繁就必須把它收斂成恰好一支,
     * 不能只處理「乾淨的起始狀態」。
     */
    @Test
    fun `髒狀態同組兩支同時開會被收斂成恰好一支`() {
        val dirty = setOf(TW, HANS)

        val toTrad = VariantPlan.plan(dirty, null, false)
        assertEquals("髒狀態要繁體,應收斂回臺灣字形", listOf(TW), onesAfter(dirty, toTrad))

        val toSimp = VariantPlan.plan(dirty, null, true)
        assertEquals("髒狀態要簡體,應只剩 zh_hans", listOf(HANS), onesAfter(dirty, toSimp))
    }

    @Test
    fun `髒狀態下要繁體時記憶取的是繁體那一支而不是zh_hans`() {
        val plan = VariantPlan.plan(setOf(HK, HANS), null, false)
        assertEquals(HK, plan.remembered)
    }

    // ── 全關:方案沒有 reset 的情況也要有定義 ────────────────────────────

    /**
     * luna_pinyin 的 switches 沒有 `reset:`,所以新 session 一開始同組**全是假**。
     * 這時「原本那一支」不存在,也沒有記憶可用 —— 必須有一個講得出理由的預設,
     * 而不是什麼都不做(什麼都不做 = 使用者選了繁體卻沒有任何一支為真)。
     */
    @Test
    fun `四支全關要繁體時退到傳統漢字`() {
        val plan = VariantPlan.plan(emptySet(), null, false)
        assertEquals(listOf(HANT), onesAfter(emptySet(), plan))
    }

    @Test
    fun `四支全關要簡體時給zh_hans`() {
        val plan = VariantPlan.plan(emptySet(), null, true)
        assertEquals(listOf(HANS), onesAfter(emptySet(), plan))
    }

    @Test
    fun `四支全關但記得臺灣字形時要繁體會還原臺灣字形`() {
        val plan = VariantPlan.plan(emptySet(), TW, false)
        assertEquals(listOf(TW), onesAfter(emptySet(), plan))
    }

    // ── 順序是契約的一部分,與 Windows 的 PlanVariant 一致 ────────────────

    /**
     * 「先關再開」要寫進**順序**裡。靠「應該看不到中間態」是靠不住的:
     * rs_set_option 每一次呼叫都可能讓引擎重新算候選,中間態是看得到的。
     */
    @Test
    fun `序列固定五項_simplification在最前_目標那一支在最後`() {
        for (start in startStates) {
            for (want in listOf(true, false)) {
                val plan = VariantPlan.plan(start, TW, want)
                assertEquals("起始=$start 要簡體=$want", 5, plan.assigns.size)

                val head = plan.assigns.first()
                assertEquals(VariantPlan.OPT_SIMPLIFICATION, head.option)
                assertEquals(want, head.value)

                val tail = plan.assigns.last()
                assertTrue("最後一項應該是唯一設成 true 的字形", tail.value)
                assertTrue(tail.option in VariantPlan.VARIANT_OPTIONS)

                val middle = plan.assigns.subList(1, 4)
                assertTrue("中間三項都該是關", middle.none { it.value })
                assertTrue(
                    "中間三項應該正好是同組其餘三支",
                    middle.map { it.option }.toSet() ==
                        VariantPlan.VARIANT_OPTIONS.toSet() - tail.option,
                )
            }
        }
    }

    /** 同一份計畫套第二次不會有任何改變 —— 重新部署、session 重建都會重套。 */
    @Test
    fun `重複套用同一份計畫不改變結果`() {
        for (start in startStates) {
            for (want in listOf(true, false)) {
                val plan = VariantPlan.plan(start, TW, want)
                val once = VariantPlan.applyTo(start, plan.assigns)
                val twice = VariantPlan.applyTo(once, plan.assigns)
                assertEquals(once, twice)
            }
        }
    }

    /** 已經在要的狀態上,重算出來的計畫不能把它推走。 */
    @Test
    fun `已經是目標狀態時重算計畫仍停在原地`() {
        val plan = VariantPlan.plan(setOf(TW), TW, false)
        assertEquals(listOf(TW), onesAfter(setOf(TW), plan))
        assertNotNull(plan.remembered)
    }

    /** 選項名就是 luna_pinyin.schema.yaml 的那四個,拼錯的話真機上完全沒有徵兆。 */
    @Test
    fun `選項名與方案宣告的那一組相同`() {
        assertEquals(
            listOf("zh_hans", "zh_hant", "zh_hant_hk", "zh_hant_tw"),
            VariantPlan.VARIANT_OPTIONS,
        )
        assertEquals(listOf("zh_hant", "zh_hant_hk", "zh_hant_tw"), VariantPlan.TRADITIONAL_OPTIONS)
        assertEquals("simplification", VariantPlan.OPT_SIMPLIFICATION)
    }

    // ⚠ 這三條守的是**呼叫端給了髒東西**的情況。今天 Android 走不到,
    //   但這份模型明文要另外三端照抄,而且 remembered 一旦落盤就會讀到舊字串。
    //   沒有這三條的話,那個掃描不變式的測試只餵四個合法值 —— 等於沒有守。
    @Test
    fun `記憶是 zh_hans 時要繁體不可以又選回 zh_hans`() {
        val p = VariantPlan.plan(emptySet(), VariantPlan.OPT_ZH_HANS, wantSimplified = false)
        val after = VariantPlan.applyTo(emptySet(), p.assigns)
        assertEquals(setOf(VariantPlan.OPT_ZH_HANT), after)
        assertEquals(VariantPlan.OPT_ZH_HANT, p.remembered ?: VariantPlan.OPT_ZH_HANT)
    }

    @Test
    fun `記憶是組外的字串時要繁體仍然恰好開一支`() {
        for (junk in listOf("zh_hant_sg", "", "  ", "simplification", "ZH_HANT")) {
            val p = VariantPlan.plan(emptySet(), junk, wantSimplified = false)
            val after = VariantPlan.applyTo(emptySet(), p.assigns)
            assertEquals(
                "記憶=\"$junk\" 時同組要恰好一支為真",
                setOf(VariantPlan.OPT_ZH_HANT),
                after,
            )
        }
    }

    @Test
    fun `髒記憶不會被原樣傳下去`() {
        for (junk in listOf(VariantPlan.OPT_ZH_HANS, "zh_hant_sg", "")) {
            val p = VariantPlan.plan(emptySet(), junk, wantSimplified = true)
            assertTrue(
                "記憶=\"$junk\" 不可以原樣留在 Plan.remembered 裡",
                p.remembered == null || p.remembered in VariantPlan.TRADITIONAL_OPTIONS,
            )
        }
    }
}
