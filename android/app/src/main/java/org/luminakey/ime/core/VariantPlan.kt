package org.luminakey.ime.core

/**
 * 簡繁字形:給「同組現在哪幾支開著」+「使用者要什麼」,產出 set_option 的序列。
 *
 * ── 為什麼需要這一支 ──────────────────────────────────────────────────
 *
 * 隨附的 luna_pinyin 家族**沒有** `simplification` 開關,它用一組互斥的 radio:
 *
 * ```yaml
 * - options: [ zh_hant, zh_hans, zh_hant_hk, zh_hant_tw ]
 *   states:  [ 傳統漢字, 简化字,  香港字形,   臺灣字形 ]
 * ```
 *
 * 而 `rs_set_option` **不維持 radio 的互斥** —— 維持互斥的是 librime 的 switcher,
 * 只有使用者從方案選單裡選的時候才會跑。程式自己設的話,同組可以同時有兩支為真,
 * opencc 於是把 t2s 之後的結果再串一次 t2tw,候選列就簡繁混雜。
 *
 * 使用者實機回報的原話:「無論是簡體還是繁體 打出來候選詞都是一樣的
 * 有繁體 有簡體」。在 emulator-5558 上用 rime_console 對 luna_pinyin_tw 打
 * `guojia` 實測:
 *
 * ```
 * 只開 zh_hant_tw            → 國家 郭嘉 國際 國籍 過激
 * zh_hant_tw + zh_hans 都開  → 国家 郭嘉 国际 国籍 过激   ← 使用者看到的
 * 只開 zh_hans               → 国家 郭嘉 国际 国籍 过激
 * ```
 *
 * ── 為什麼是純函式 ────────────────────────────────────────────────────
 *
 * 這段邏輯原本埋在 `RimeInputMethodService.applyVariant` 裡,要驗它得先有
 * session、有引擎、有詞庫 —— 也就是只有真機跑得動,而真機不在 CI 上。
 * 抽出來之後,「同組恰好一支為真」這個不變式在 JVM 單元測試裡守得住,
 * 而且四端共用同一個模型:Windows 的 `PlanVariant`
 * (windows/common/schema_choice.h)已經是這個形狀,**行為要一致**。
 *
 * Windows 那一邊挑「哪一種繁體」是看 TSF 的 langid(香港的使用者拿香港字形);
 * Android 沒有 langid,改看**使用者原本開著的是哪一支**,由 [Plan.remembered]
 * 帶著走。決策來源不同,產出的序列形狀相同。
 */
object VariantPlan {

    const val OPT_SIMPLIFICATION = "simplification"
    const val OPT_ZH_HANS = "zh_hans"
    const val OPT_ZH_HANT = "zh_hant"
    const val OPT_ZH_HANT_HK = "zh_hant_hk"
    const val OPT_ZH_HANT_TW = "zh_hant_tw"

    /** 方案宣告的那一組,順序照 `luna_pinyin.schema.yaml`。 */
    val VARIANT_OPTIONS = listOf(OPT_ZH_HANS, OPT_ZH_HANT, OPT_ZH_HANT_HK, OPT_ZH_HANT_TW)

    /** 同組裡屬於「繁體」的那幾支。`zh_hans` 以外的全部。 */
    val TRADITIONAL_OPTIONS = listOf(OPT_ZH_HANT, OPT_ZH_HANT_HK, OPT_ZH_HANT_TW)

    /** 一次 `rs_set_option`。順序有意義,見 [plan]。 */
    data class Assign(val option: String, val value: Boolean)

    /**
     * @param assigns 要照**這個順序**逐項送出去的 set_option。
     * @param remembered 更新後的「使用者的繁體選擇」,呼叫端要存回去。
     *   ⚠ 它永遠不會是 [OPT_ZH_HANS] —— 見 [plan] 的說明。
     */
    data class Plan(val assigns: List<Assign>, val remembered: String?)

    /**
     * @param current 同組裡現在為真的選項(其餘的視為假)。
     * @param remembered 上一次記住的繁體選擇,還沒有就給 null。
     * @param wantSimplified 使用者要簡體嗎。
     *
     * ── 產出的序列 ────────────────────────────────────────────────────
     *
     * 固定五項,與 Windows 的 `PlanVariant` 逐項對齊:
     *
     * ```
     * 1.   simplification = wantSimplified   真的有這個開關的方案(第三方的
     *                                       五筆·簡入繁出之類)靠它;沒有的無視它
     * 2-4. 同組其餘三支     = false          ← 先關
     * 5.   目標那一支       = true           ← 再開
     * ```
     *
     * **「先關再開」寫進順序裡,而不是靠「應該看不到中間態」。** 每一次
     * `rs_set_option` 都可能讓引擎重算候選,中間態是看得到的;而「全關」
     * 這個中間態是安全的(退回方案自己的預設輸出),「兩支同時開」不是。
     *
     * 五項是無條件送的,不做「跟現在一樣就跳過」的最佳化:少送一項就等於
     * 相信 [current] 是準的,而**這個缺陷的成因正是狀態與預期不符**。
     *
     * ── 切回繁體時還原成哪一支 ────────────────────────────────────────
     *
     * 還原**原本那一支**,不是硬設 `zh_hant`:使用者原本在「臺灣字形」,
     * 切一趟簡體再回來卻變成「傳統漢字」,字形會整批改變,那不是他要求的。
     *
     * ⚠ 「原本那一支」只從 [TRADITIONAL_OPTIONS] 裡找,**不能包含 zh_hans**。
     *   舊版用 `VARIANT_OPTIONS.firstOrNull { 開著 }` 算,而 `zh_hans` 排在
     *   清單最前面 —— 一旦處在簡體狀態,算出來的就是 `zh_hans` 自己,記憶
     *   當場被自己弄髒,於是「切回繁體」變成「把 zh_hans 再設一次 true」,
     *   狀態不變,**切過一次簡體就永遠回不到繁體**。那正是使用者回報的症狀。
     *
     * 現在為真的繁體支優先於 [remembered](使用者可能剛從方案選單裡改過字形,
     * 那比我們上次記下的新);兩者都沒有就退到 `zh_hant`。
     *
     * ⚠ 「都沒有」是真的會發生的:luna_pinyin 的 switches 沒有 `reset:`,
     *   新 session 一開始同組全是假。這時什麼都不做的話,使用者選了繁體
     *   卻沒有任何一支為真 —— 所以要有一個講得出理由的預設。
     */
    fun plan(current: Set<String>, remembered: String?, wantSimplified: Boolean): Plan {
        // ⚠ 只看繁體那三支。看整組的話,簡體狀態下會把 zh_hans 記成「原本那一支」。
        val nextRemembered = TRADITIONAL_OPTIONS.firstOrNull { it in current } ?: remembered

        val target = if (wantSimplified) OPT_ZH_HANS else (nextRemembered ?: OPT_ZH_HANT)

        val assigns = mutableListOf<Assign>()
        assigns += Assign(OPT_SIMPLIFICATION, wantSimplified)
        for (option in VARIANT_OPTIONS) {
            if (option != target) assigns += Assign(option, false)   // 先關
        }
        assigns += Assign(target, true)                              // 再開

        return Plan(assigns, nextRemembered)
    }

    /**
     * 把一份計畫套到一個狀態上,回傳同組新的「哪幾支為真」。
     *
     * 給測試用的模型,也是 [plan] 產出的序列在引擎裡會發生什麼事的定義。
     * `simplification` 不屬於這一組,跳過。
     */
    fun applyTo(current: Set<String>, assigns: List<Assign>): Set<String> {
        val next = current.toMutableSet()
        for (a in assigns) {
            if (a.option !in VARIANT_OPTIONS) continue
            if (a.value) next += a.option else next -= a.option
        }
        return next
    }
}
