package org.luminakey.ime.prefs

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

    val CANDIDATE_COUNT_LABELS = listOf("3 個", "5 個", "7 個", "9 個", "不限")
    private val CANDIDATE_COUNTS = listOf(3, 5, 7, 9, 0)

    fun indexOfCandidateCount(prefs: UserPrefs, baseCount: Int): Int {
        val n = prefs.candidateCount ?: baseCount
        if (n <= 0) return 4
        return nearest(CANDIDATE_COUNTS.dropLast(1).map { it.toFloat() }, n.toFloat())
    }

    fun withCandidateCount(prefs: UserPrefs, index: Int): UserPrefs =
        prefs.copy(candidateCount = CANDIDATE_COUNTS[index.coerceIn(0, 4)])

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
