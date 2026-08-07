package org.rimequad.ime.prefs

import android.content.Context
import android.media.AudioManager
import android.view.HapticFeedbackConstants
import android.view.View
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.platform.ViewConfiguration
import org.rimequad.ime.theme.Feedback
import org.rimequad.ime.theme.HapticStrength

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
class KeyBehavior(
    val haptic: Boolean,
    val hapticStrength: HapticStrength,
    val sound: Boolean,
    val soundVolume: Float,
    val longPressMs: Int,
    val repeatDelayMs: Int,
    val repeatIntervalMs: Int,
) {

    /**
     * 按鍵當下的回饋。由 `KeyboardView` 在每一次觸發時呼叫。
     *
     * 震動走 [View.performHapticFeedback] 而不是 `Vibrator`：後者需要
     * `android.permission.VIBRATE`，為了三段強度去要一個執行期權限
     * 划不來，而且系統的觸覺常數在各家 ROM 上調校得比自訂波形好。
     * 代價是「強度」只能映射到三個粗糙的常數，見下表。
     */
    fun onKeyPress(view: View) {
        if (haptic && hapticStrength != HapticStrength.NONE) {
            view.performHapticFeedback(
                when (hapticStrength) {
                    HapticStrength.LIGHT -> HapticFeedbackConstants.CLOCK_TICK
                    HapticStrength.HEAVY -> HapticFeedbackConstants.LONG_PRESS
                    else -> HapticFeedbackConstants.KEYBOARD_TAP
                },
                HapticFeedbackConstants.FLAG_IGNORE_GLOBAL_SETTING,
            )
        }
        if (sound && soundVolume > 0f) {
            val am = view.context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
            am?.playSoundEffect(AudioManager.FX_KEYPRESS_STANDARD, soundVolume.coerceIn(0f, 1f))
        }
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
        fun of(feedback: Feedback, prefs: UserPrefs): KeyBehavior = KeyBehavior(
            haptic = feedback.haptic,
            hapticStrength = feedback.hapticStrength,
            sound = feedback.sound,
            soundVolume = feedback.soundVolume,
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
class LongPressViewConfiguration(
    private val base: ViewConfiguration,
    private val longPressMs: Long,
) : ViewConfiguration by base {
    override val longPressTimeoutMillis: Long get() = longPressMs
}
