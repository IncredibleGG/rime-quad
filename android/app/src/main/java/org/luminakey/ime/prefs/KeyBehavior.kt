package org.luminakey.ime.prefs

import android.view.View
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.platform.ViewConfiguration
import org.luminakey.ime.core.FeedbackPlan
import org.luminakey.ime.core.KeyRole
import org.luminakey.ime.core.SoundTimbre
import org.luminakey.ime.theme.Feedback
import org.luminakey.ime.theme.HapticStrength

/**
 * 按鍵的**行為**參數 —— 主題格式覆蓋不到的那一部分。
 *
 * ⚠ 規範缺口（已在回報中列出）：`docs/theme-format.md` 定義了
 * `feedback.haptic` / `haptic_strength` / `sound` / `sound_volume`（§8.10），
 * 也定義了佈局的 `repeat: bool` 與 `long_press: action`（§9.7），
 * **但沒有定義任何時間量**：長按要按多久才算長按、按住之後多久開始重複、
 * 重複的間隔是多少，格式裡一個欄位都沒有。
 *
 * 因此這三個值的「未設定」不可能退回主題檔 —— 沒有那個欄位可退。
 * 它們退回本檔的 [DEFAULT]，也就是 `KeyboardView` 原本寫死的那組數字
 * （400 / 60）與 Android 的系統長按門檻。這是誠實的降級，不是把預設值
 * 偽裝成主題值。
 *
 * [KeyBehavior] 同時把 [Feedback] 的四個欄位帶進來，讓 `KeyboardView`
 * 只需要認得一個物件就能做完震動與按鍵音。
 */
@Immutable
data class KeyBehavior(
    val haptic: Boolean,
    val hapticStrength: HapticStrength,
    val sound: Boolean,
    val soundVolume: Float,
    /** 按鍵音的音色。與音量是兩個獨立的設定,見 [SoundTimbre]。 */
    val soundTimbre: SoundTimbre,
    /**
     * 這支手機的馬達分不分得出強弱(`Vibrator.hasAmplitudeControl()`)。
     *
     * 它不是偏好,是**裝置能力** —— 放在這裡是因為它與四階強度一起決定
     * 「要送出什麼」,而那個決定必須是一個純函式([FeedbackPlan.haptic])。
     * 由服務端量一次之後帶進來。
     */
    val amplitudeControl: Boolean,
    val longPressMs: Int,
    val repeatDelayMs: Int,
    val repeatIntervalMs: Int,
) {

    /**
     * 按鍵當下的回饋。由 `KeyboardView` 在每一次觸發時呼叫。
     *
     * ── 2026-08-12:這一段整個被拆成三塊 ───────────────────────────────
     * 在此之前它是一坨:三個觸覺常數 + 一律 `FX_KEYPRESS_STANDARD`,而且
     * 帶著 `FLAG_IGNORE_GLOBAL_SETTING`。三個問題:
     *
     *   1. 三個常數不是同一個波形的三種大小,是三支不相干的波形
     *      (實測「強」比「中」**短三倍**)。使用者要的「大小」不存在。
     *   2. 一律 STANDARD —— 連系統免費給的四個角色都沒有用上。
     *   3. 使用者已經在系統設定裡關掉觸覺回饋,我們照震不誤。
     *
     * 現在:算計畫([FeedbackPlan],純函式,測得到)→ 交給兩個薄薄的出口
     * ([KeyHaptics] / [KeySounds])。這一支只剩接線。
     *
     * @param role 這一顆鍵算哪一種(空白／刪除／換行／一般)。由呼叫端從
     *   佈局的 keysym 算出來,見 [FeedbackPlan.roleOf]。
     */
    fun onKeyPress(view: View, role: KeyRole = KeyRole.STANDARD) {
        val context = view.context
        KeyHaptics.play(
            context,
            view,
            FeedbackPlan.haptic(haptic, hapticStrength, amplitudeControl),
        )
        KeySounds.play(
            context,
            FeedbackPlan.sound(sound, soundVolume, soundTimbre, role),
        )
    }

    companion object {
        /** `KeyboardView` 原本寫死的值，也是「沒有主題欄位可退」時的落點。 */
        const val DEFAULT_LONG_PRESS_MS = 500
        const val DEFAULT_REPEAT_DELAY_MS = 400
        const val DEFAULT_REPEAT_INTERVAL_MS = 60

        /**
         * 沒有主題也沒有偏好時的行為。刻意與改動前的 `KeyboardView` 完全一致，
         * 這樣即使 CompositionLocal 沒有被提供（例如預覽、或未來有人直接
         * 呼叫 `RimeKeyboard`），鍵盤的手感也不會變。
         */
        val DEFAULT = KeyBehavior(
            haptic = true,
            hapticStrength = HapticStrength.MEDIUM,
            sound = false,
            soundVolume = 0.3f,
            soundTimbre = SoundTimbre.SYSTEM,
            // ⚠ 沒有人提供這個 CompositionLocal 時,一律當作「這支手機沒有
            //    振幅控制」—— 於是走的是舊的常數路徑,行為與改動前一致
            //    (唯一的差別是不再帶 FLAG_IGNORE_GLOBAL_SETTING,那是刻意的)。
            //    寧可把可調的那條路留給「真的量過」的呼叫端。
            amplitudeControl = false,
            longPressMs = DEFAULT_LONG_PRESS_MS,
            repeatDelayMs = DEFAULT_REPEAT_DELAY_MS,
            repeatIntervalMs = DEFAULT_REPEAT_INTERVAL_MS,
        )

        /**
         * 由已套用覆寫的主題 + 使用者偏好求值。
         *
         * [feedback] 已經過 [applyUserOverrides]，所以這裡不再讀
         * `prefs.hapticEnabled` 之類的欄位 —— 只有一個地方決定回饋設定，
         * 免得兩處邏輯漂移。
         */
        fun of(
            feedback: Feedback,
            prefs: UserPrefs,
            amplitudeControl: Boolean = false,
        ): KeyBehavior = KeyBehavior(
            haptic = feedback.haptic,
            hapticStrength = feedback.hapticStrength,
            sound = feedback.sound,
            soundVolume = feedback.soundVolume,
            // 音色沒有主題欄位可退(見 UserPrefs.soundTimbre),所以它不經過
            // applyUserOverrides,直接讀偏好。
            soundTimbre = prefs.soundTimbre ?: SoundTimbre.SYSTEM,
            amplitudeControl = amplitudeControl,
            longPressMs = (prefs.longPressMs ?: DEFAULT_LONG_PRESS_MS).coerceIn(150, 1200),
            repeatDelayMs = (prefs.repeatDelayMs ?: DEFAULT_REPEAT_DELAY_MS).coerceIn(150, 1200),
            repeatIntervalMs =
                (prefs.repeatIntervalMs ?: DEFAULT_REPEAT_INTERVAL_MS).coerceIn(20, 400),
        )
    }
}

/**
 * `KeyboardView` 取用行為參數的唯一管道。
 *
 * 用 CompositionLocal 而不是多加一個參數，是為了把對渲染層的改動壓到
 * 最小 —— `RimeKeyboard` 的簽章不變，只有 `KeyView` 內部多讀一個值。
 */
val LocalKeyBehavior = staticCompositionLocalOf { KeyBehavior.DEFAULT }

/**
 * 只換掉長按門檻的 [ViewConfiguration] 代理。
 *
 * `detectTapGestures` 的 `onLongPress` 用的是 `PointerInputScope.viewConfiguration
 * .longPressTimeoutMillis`，而那個值來自 `LocalViewConfiguration`。所以覆寫
 * 長按延遲**不需要動 KeyboardView 一行**：在 `RimeKeyboard` 外面把這個代理
 * 提供進去就好。
 */
@Immutable
class LongPressViewConfiguration(
    private val base: ViewConfiguration,
    private val longPressMs: Long,
) : ViewConfiguration by base {
    override val longPressTimeoutMillis: Long get() = longPressMs

    // ⚠ 這兩支不是樣板碼，是一個**視覺 bug 的修法**，刪掉會讓它復發。
    //
    // `LocalViewConfiguration` 的值一換，Compose 就會對整棵樹裡每一個
    // `Modifier.pointerInput` 呼叫 `onViewConfigurationChange()`，
    // 而那支的實作是 `resetPointerInputHandler()` —— 手指還按著的那一顆鍵，
    // 它的手勢協程當場被取消。
    //
    // 換句話說：只要這個物件每次重組都是新的一份，使用者**每按一顆鍵**
    // 就會把自己按下的那顆鍵的手勢協程殺掉。回報過的「點一下就變灰、
    // 變不回來」就是這麼來的（見 KeyboardViewPressStateTest 的檔頭）。
    //
    // 所以它必須有值相等性：長按門檻沒變、base 沒變，就是同一份設定，
    // Compose 不該把它當成新值。
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is LongPressViewConfiguration) return false
        return base == other.base && longPressMs == other.longPressMs
    }

    override fun hashCode(): Int = 31 * base.hashCode() + longPressMs.hashCode()
}
