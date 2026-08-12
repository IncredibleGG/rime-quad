package org.luminakey.ime.prefs

import org.luminakey.ime.core.SoundTimbre
import org.luminakey.ime.theme.HapticStrength
import kotlin.math.abs

/**
 * 把幾個連續值的偏好收成**四個明確的檔位**。
 *
 * ── 為什麼不是滑桿 ──────────────────────────────────────────────────────
 * 手感沒有人分得出 37% 和 41% 的差別；候選字級也沒有人想要 1.15 倍。
 * 給四個檔位反而更快決定，而且畫面上再也不必出現百分比、毫秒或倍數 ——
 * 使用者要的是「再大一點」，不是 `1.15`。
 *
 * ── 為什麼這一檔是純函式 ────────────────────────────────────────────────
 * 同一組檔位有兩個消費端：App 的第二層設定頁，與鍵盤上的就地編輯器。
 * 兩邊各寫一份對照表，遲早會漂移成兩套不同的「中」。這裡是唯一的一份，
 * 而且不碰 Android、不碰 Compose，可以直接單元測試（見 PrefLevelsTest）。
 *
 * ── 「未設定」怎麼顯示 ──────────────────────────────────────────────────
 * [UserPrefs] 的第一原則是不把預設值抄一份進偏好，所以未設定時要拿**主題檔
 * 的值**去反推現在停在哪一檔（`indexOf*` 的 `base*` 參數）。反推不到最近的
 * 一檔就取最近的 —— 顯示成一個檔位不精確，但總比顯示成「無」好，那會讓
 * 使用者以為按鍵音是關的。
 */
object PrefLevels {

    /* ─────────────────────── 按鍵音 ─────────────────────── */

    val SOUND_LABELS = listOf("關", "小", "中", "大")
    private val SOUND_VOLUMES = listOf(0f, 0.30f, 0.65f, 1.0f)

    fun indexOfSound(prefs: UserPrefs, baseEnabled: Boolean, baseVolume: Float): Int {
        val on = prefs.soundEnabled ?: baseEnabled
        if (!on) return 0
        val v = prefs.soundVolume ?: baseVolume
        return nearest(SOUND_VOLUMES, v).coerceAtLeast(1)
    }

    fun withSound(prefs: UserPrefs, index: Int): UserPrefs =
        if (index <= 0) prefs.copy(soundEnabled = false)
        else prefs.copy(soundEnabled = true, soundVolume = SOUND_VOLUMES[index.coerceAtMost(3)])

    /* ─────────────────────── 按鍵音色 ─────────────────────── */

    /**
     * 音量與音色是**兩個**控制項,不是一個八格的。
     *
     * 合成一個的話會有 8 個選項(違反 `docs/ui-design.md` §4.2 的「分段控制
     * 2–4 格」),而且「關」該屬於哪一邊會說不清 —— 關掉之後音色還在不在?
     */
    val TIMBRE_LABELS = listOf("系統", "輕點", "敲擊", "水滴")
    private val TIMBRES = listOf(
        SoundTimbre.SYSTEM, SoundTimbre.SOFT, SoundTimbre.MECHANICAL, SoundTimbre.DROP,
    )

    fun indexOfTimbre(prefs: UserPrefs): Int =
        TIMBRES.indexOf(prefs.soundTimbre ?: SoundTimbre.SYSTEM).coerceAtLeast(0)

    /**
     * @param soundLevel 目前的按鍵音檔位([indexOfSound] 的回傳值)。
     *
     * ⚠ 「按鍵音=關」的時候點音色會**順便把音量打開到「小」**。
     *
     * 這是刻意的。另外兩種做法都比較差:畫成灰的違反 §1「做不到的功能不要
     * 畫出來」(它做得到,只是要先開音量);隱藏起來會讓版面跳。而一個
     * 「點下去什麼都沒發生」的控制項是這個專案抓過好幾次的那一類缺陷。
     *
     * 一次操作取代兩次,而且沒有死控制項。
     */
    fun withTimbre(prefs: UserPrefs, index: Int, soundLevel: Int): UserPrefs {
        val next = prefs.copy(soundTimbre = TIMBRES[index.coerceIn(0, TIMBRES.size - 1)])
        return if (soundLevel <= 0) withSound(next, 1) else next
    }

    /* ─────────────────────── 震動 ─────────────────────── */

    val HAPTIC_LABELS = listOf("關", "弱", "中", "強")
    private val HAPTIC_STRENGTHS =
        listOf(HapticStrength.LIGHT, HapticStrength.LIGHT, HapticStrength.MEDIUM, HapticStrength.HEAVY)

    fun indexOfHaptic(prefs: UserPrefs, baseEnabled: Boolean, baseStrength: HapticStrength): Int {
        val on = prefs.hapticEnabled ?: baseEnabled
        if (!on) return 0
        return when (prefs.hapticStrength ?: baseStrength) {
            HapticStrength.NONE -> 0
            HapticStrength.LIGHT -> 1
            HapticStrength.MEDIUM -> 2
            HapticStrength.HEAVY -> 3
        }
    }

    fun withHaptic(prefs: UserPrefs, index: Int): UserPrefs =
        if (index <= 0) prefs.copy(hapticEnabled = false)
        else prefs.copy(
            hapticEnabled = true,
            hapticStrength = HAPTIC_STRENGTHS[index.coerceAtMost(3)],
        )

    /* ─────────────────────── 長按多久算長按 ─────────────────────── */

    val LONG_PRESS_LABELS = listOf("快", "標準", "慢", "更慢")
    private val LONG_PRESS_MS = listOf(250, 400, 600, 800)

    fun indexOfLongPress(prefs: UserPrefs): Int =
        nearest(LONG_PRESS_MS.map { it.toFloat() }, (prefs.longPressMs ?: KeyBehavior.DEFAULT_LONG_PRESS_MS).toFloat())

    fun withLongPress(prefs: UserPrefs, index: Int): UserPrefs =
        prefs.copy(longPressMs = LONG_PRESS_MS[index.coerceIn(0, 3)])

    /* ─────────────────────── 一次顯示幾個候選 ─────────────────────── */

    /**
     * 引擎一頁最多給幾個候選。
     *
     * **來源只有一份:`core/data/shared/default.yaml` 的 `menu/page_size`。**
     * 這裡不是抄過來的 —— [ENGINE_PAGE_SIZE_FROM_DATA] 是建置期從那份 yaml
     * 產生的（`android/app/build.gradle.kts` 的 `generateEnginePageSize`），
     * 讀不到就不給預設值、直接讓建置失敗。
     *
     * 這個數字是**上限**:`rs_snapshot` 的 `menu.items` 一頁就這麼多，
     * 前端的 `max_visible` 只能從裡面挑得更少，挑不出更多。
     *
     * ── 為什麼不可以寫死 ──────────────────────────────────────────────
     * 從前這裡是一個手寫的 `= 5`,而抄本**不知道正本改了**。實際發生過:
     * 一條線量到引擎給 5 個,於是把檔位砍成 3/4/5 並寫下常數 5;另一條線
     * 同時把 `menu/page_size` 改成 9。兩邊各自都對,合起來使用者拿到的是
     * 「畫面畫 9 個、設定列卻顯示 5 個,碰一下就永久鎖在 5、回不到 9」——
     * 而底下那條守著檔位的測試**不會紅**,因為 3/4/5 確實都 ≤ 9。
     *
     * ⚠ 第三方方案可以在自己的 schema 裡覆寫 `menu/page_size`。那時這個常數
     *   說的是「隨附資料的預設」,不是那個方案的實際值。後果是良性的:
     *   `take(cap)` 拿到比較少的候選,不錯位也不當掉 —— 「選了 9 但這個方案
     *   只給得出 5」與從前那個「選了 9 而**任何**方案都只給 5」不是同一件事。
     *   為什麼不改成執行期問引擎,理由寫在 build.gradle.kts 那一段。
     */
    const val ENGINE_PAGE_SIZE = ENGINE_PAGE_SIZE_FROM_DATA

    /**
     * ── 這幾檔曾經被砍掉,而砍的理由在資料改了之後就不成立了 ────────────
     * 批 1 量到引擎一頁只給 5 個,7 / 9 / 不限三檔**按了沒反應**,於是砍成
     * 3/4/5。那個判斷在當時是對的。候選詞那條線隨後把 `menu/page_size`
     * 改成 9 —— 引擎真的給得出 9 個了,砍掉的理由就沒了,而檔位沒有跟著回來。
     *
     * 現在檔位跟著 [ENGINE_PAGE_SIZE] 走:
     *
     *   · 每一檔都 ≤ [ENGINE_PAGE_SIZE],所以每一檔都真的畫得出不同的數量
     *     ——「一個按了沒反應的設定比沒有這個設定更糟」那條規矩仍然在。
     *   · **最後一檔正好等於 [ENGINE_PAGE_SIZE]**,所以引擎給得出來的
     *     使用者一定選得到。只守「≤」的話,`page_size` 調大之後最後一檔會
     *     安靜地留在舊值,而畫面上看不出任何異常 —— 那是同一個缺陷的鏡像。
     *     `EnginePageSizeTest` 兩邊都釘住。
     *
     * ⚠ **這裡刻意寫成字面值,不是 `listOf(3, 5, 7, ENGINE_PAGE_SIZE)`。**
     *   寫成後者的話「最後一檔等於引擎那一頁」永遠成立,那條測試就變成
     *   套套邏輯 —— 而畫面上的標籤(三份 `strings.xml` 的
     *   `levels_candidate_count`)是**靜態字串**,不會跟著浮動。
     *   `page_size` 哪天變成 11,值會變成 11 而標籤還寫著「9 個」,
     *   使用者看到的仍然是一個對他說謊的設定列。
     *
     *   字面值 + `EnginePageSizeTest` 的等式,`page_size` 一改就紅,
     *   而紅的訊息會說「檔位與三份 strings.xml 要一起改」。
     *   數字要在兩個地方出現,那就讓測試逼它們一致,不要讓其中一邊安靜地漂。
     *
     * 「不限」沒有跟著回來:`max_visible: 0` 畫出來就是一整頁,與最後一檔
     * (正好一整頁)在畫面上是同一件事,而「不限」會讓使用者以為還能更多。
     */
    val CANDIDATE_COUNT_LABELS = listOf("3 個", "5 個", "7 個", "9 個")
    private val CANDIDATE_COUNTS = listOf(3, 5, 7, 9)

    fun indexOfCandidateCount(prefs: UserPrefs, baseCount: Int): Int {
        val n = prefs.candidateCount ?: baseCount
        // n <= 0 是主題的「有多少畫多少」（`max_visible: 0`），實際就是一整頁,
        // 而一整頁正好是最後一檔。n 比最後一檔還大(舊版存下來的 99、或第三方
        // 主題寫了一個大數)也落在最後一檔:那是它實際畫得出來的樣子。
        if (n <= 0 || n >= CANDIDATE_COUNTS.last()) return CANDIDATE_COUNTS.size - 1
        return nearest(CANDIDATE_COUNTS.map { it.toFloat() }, n.toFloat())
    }

    fun withCandidateCount(prefs: UserPrefs, index: Int): UserPrefs =
        prefs.copy(
            candidateCount = CANDIDATE_COUNTS[index.coerceIn(0, CANDIDATE_COUNTS.size - 1)],
        )

    /* ─────────────────────── 候選字多大 ─────────────────────── */

    val CANDIDATE_SIZE_LABELS = listOf("小", "標準", "大", "很大")
    private val CANDIDATE_SIZES = listOf(0.85f, 1.0f, 1.2f, 1.45f)

    fun indexOfCandidateSize(prefs: UserPrefs): Int =
        nearest(CANDIDATE_SIZES, prefs.candidateSizeScale ?: 1.0f)

    fun withCandidateSize(prefs: UserPrefs, index: Int): UserPrefs =
        prefs.copy(candidateSizeScale = CANDIDATE_SIZES[index.coerceIn(0, 3)])

    /* ─────────────────────── 共用 ─────────────────────── */

    /** 離 [value] 最近的那一檔。等距時取小的一檔（比較保守）。 */
    internal fun nearest(steps: List<Float>, value: Float): Int {
        var best = 0
        var bestDelta = Float.MAX_VALUE
        steps.forEachIndexed { i, s ->
            val d = abs(s - value)
            if (d < bestDelta) {
                bestDelta = d
                best = i
            }
        }
        return best
    }
}
