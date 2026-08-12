package org.luminakey.ime.prefs

import android.content.Context
import android.media.AudioAttributes
import android.os.Build
import android.os.VibrationAttributes
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.view.HapticFeedbackConstants
import android.view.View
import org.luminakey.ime.BuildConfig
import org.luminakey.ime.core.FeedbackPlan

/**
 * 全 app **唯一**碰得到 `Vibrator` / `VibrationEffect` 的地方。
 *
 * ── 為什麼要有「唯一出口」這個紀律 ──────────────────────────────────────
 * 這個專案已經有一個一模一樣的東西:連網只能走 `net/NetworkGate.kt`,
 * 而且 `scripts/audit_offline.sh` 第 1 項會 grep 整個 `app/src` 確認沒有
 * 第二個出口。震動照同一個模子做,理由也一樣 —— 一句「我們只在按鍵時震」
 * 要能被別人用 grep 查證,不能只是我們自己說。
 *
 * `audit_offline.sh` 新增的那一項就是這一條的守門。
 *
 * ── 為什麼拿 `android.permission.VIBRATE` ────────────────────────────────
 * 舊版走 `View.performHapticFeedback` 的三個常數,而 `KeyBehavior` 的檔頭
 * 為此寫了一句理由:「後者需要 `android.permission.VIBRATE`,為了三段強度
 * 去要一個執行期權限划不來」。
 *
 * **那句話是錯的。** VIBRATE 是 `normal` 等級的安裝時權限:自動授予、
 * 不跳對話框、使用者也取消不掉。整個技術選型建立在一個假前提上。
 *
 * 而它的稽核成本近乎零 —— 它**不是資料通道**,沒有東西能透過馬達離開這台
 * 機器。對照 `AndroidManifest.xml` 為 INTERNET 寫的那二十行辯護:那段之所以
 * 難寫,是因為 INTERNET 真的會漏。VIBRATE 不會。
 *
 * (唯一真實的側信道疑慮 —— 別的 app 用加速度計從震動反推按鍵 —— 在
 * `performHapticFeedback` 底下**一模一樣存在**。我們本來就在震了,拿權限
 * 沒有新增這個風險面。)
 *
 * ── ⚠ 拿掉了 `FLAG_IGNORE_GLOBAL_SETTING` ───────────────────────────────
 * 這是交換條件,不是附帶。舊版在 `performHapticFeedback` 帶了
 * `FLAG_IGNORE_GLOBAL_SETTING`,實測(API 35, AOSP)的後果是:
 *
 *     settings put system vibrate_on 0
 *     settings put system haptic_feedback_enabled 0
 *     → dumpsys: vibrateOn = false
 *     → 我們照震不誤,狀態 finished,flags: 10
 *       (= PIPELINED | BYPASS_USER_VIBRATION_INTENSITY_OFF)
 *
 * 同一台機器上的 Gboard 是 `flags: 0`。一個賣「離線、經得起審計」的鍵盤,
 * 在使用者已經全域關掉觸覺回饋之後仍然震,比多一個 VIBRATE 難解釋得多。
 * 現在四階全部活在使用者的系統強度**之內**。
 */
object KeyHaptics {

    /**
     * 除錯建置的觀測點。**release 建置不會走到這裡**(`BuildConfig.DEBUG`)。
     *
     * 存在的理由:「選了不同的一階,真的送出了不同的東西嗎」在模擬器上
     * 沒有耳朵也沒有手 —— 但 `dumpsys vibrator_manager` 看得到振幅,logcat
     * 看得到我們**打算**送什麼。兩邊對上了才算量過。
     */
    private const val TAG = "LuminaKeyFeel"

    @Volatile
    private var cached: Vibrator? = null

    private fun vibrator(context: Context): Vibrator? {
        cached?.let { return it }
        val v = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            (context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager)
                ?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
        }
        cached = v
        return v
    }

    /**
     * 這支手機的馬達分不分得出強弱。
     *
     * false 時 [FeedbackPlan.haptic] 會退回舊的常數,而**設定頁必須把這件事
     * 講出來** —— 無聲降級等於在畫面上留三個假的檔位。
     */
    fun hasAmplitudeControl(context: Context): Boolean =
        vibrator(context)?.let { it.hasVibrator() && it.hasAmplitudeControl() } == true

    fun hasVibrator(context: Context): Boolean = vibrator(context)?.hasVibrator() == true

    /**
     * 送出一次觸覺回饋。[view] 只在退回常數那條路上用得到。
     */
    fun play(context: Context, view: View?, plan: FeedbackPlan.Haptic) {
        when (plan) {
            is FeedbackPlan.Haptic.Silent -> return

            is FeedbackPlan.Haptic.Constant -> {
                // ⚠ 這裡**沒有** FLAG_IGNORE_GLOBAL_SETTING。見檔頭。
                view?.performHapticFeedback(
                    when (plan.kind) {
                        FeedbackPlan.HapticConstant.CLOCK_TICK -> HapticFeedbackConstants.CLOCK_TICK
                        FeedbackPlan.HapticConstant.LONG_PRESS -> HapticFeedbackConstants.LONG_PRESS
                        FeedbackPlan.HapticConstant.KEYBOARD_TAP -> HapticFeedbackConstants.KEYBOARD_TAP
                    },
                )
                if (BuildConfig.DEBUG) {
                    android.util.Log.d(TAG, "haptic constant=${plan.kind}")
                }
            }

            is FeedbackPlan.Haptic.OneShot -> {
                val v = vibrator(context) ?: return
                if (!v.hasVibrator()) return
                val effect = VibrationEffect.createOneShot(
                    plan.durationMs.toLong(),
                    plan.amplitude.coerceIn(1, 255),
                )
                // 一律標成 USAGE_TOUCH,四階都一樣。
                //
                // 重點是**「一樣」**:舊版三階分別走 CLOCK_TICK / KEYBOARD_TAP /
                // LONG_PRESS,而系統只給中間那一階 `category=KEYBOARD`
                // (dumpsys 實測)。API 35 起有一個獨立的鍵盤震動開關
                // (`keyboardVibrationOn`)只管那個 category —— 於是使用者在
                // 弱↔中↔強 之間切,順便換掉了「哪一個系統開關管得到我」。
                // 現在四階同一個 usage,同一個開關。
                //
                // ⚠ 已知缺口:`VibrationAttributes.CATEGORY_KEYBOARD` **不在
                //   compileSdk 35 的公開 API 裡**(它到 API 36 才公開),所以
                //   這裡設不了。後果是在 API 35 的機器上,使用者若只關掉
                //   「鍵盤震動」而留著一般觸覺回饋,我們仍然會震 —— Gboard
                //   不會。反過來(關掉一般觸覺回饋)我們會跟著停。
                //   要補這一條得等 compileSdk 升到 36;用反射去設是不可以的,
                //   那會讓「只有 KeyHaptics 碰得到馬達」這句話變得查不出來。
                when {
                    Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU -> v.vibrate(
                        effect,
                        VibrationAttributes.Builder()
                            .setUsage(VibrationAttributes.USAGE_TOUCH)
                            .build(),
                    )

                    else -> @Suppress("DEPRECATION") v.vibrate(
                        effect,
                        AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_ASSISTANCE_SONIFICATION)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                            .build(),
                    )
                }
                if (BuildConfig.DEBUG) {
                    android.util.Log.d(
                        TAG,
                        "haptic oneshot ms=${plan.durationMs} amp=${plan.amplitude}",
                    )
                }
            }
        }
    }
}
