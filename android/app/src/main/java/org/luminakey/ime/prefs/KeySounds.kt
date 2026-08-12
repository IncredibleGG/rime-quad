package org.luminakey.ime.prefs

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.SoundPool
import android.provider.Settings
import org.luminakey.ime.BuildConfig
import org.luminakey.ime.R
import org.luminakey.ime.core.FeedbackPlan
import org.luminakey.ime.core.KeyRole
import org.luminakey.ime.core.SoundTimbre

/**
 * 按鍵音的出口。系統音效與自帶素材兩條路都從這裡走。
 *
 * ── 系統只有六支,而且它們是「角色」不是「音色」 ────────────────────────
 * `dumpsys audio` 的 Sound Effects Loading 在 AOSP 上就這六支:
 * `Effect_Tick` / `KeypressStandard` / `KeypressSpacebar` / `KeypressDelete` /
 * `KeypressReturn` / `KeypressInvalid`。它們是**同一套音效的六個角色**,
 * 設計上要一起用,不是六種可選音色 —— 做成音色選單的話,選項會叫
 * 「空白鍵音／刪除鍵音」,而 `docs/ui-design.md` §1 的介面紀律禁止把實作
 * 名詞搬上畫面。`INVALID` 是錯誤提示音,更不能當選項。
 *
 * ⚠ 而且「六支不同」只在 AOSP 上成立(那台機器上 md5 六份都不一樣)。
 *   OEM 會整套換掉,把幾個 keypress FX 指向同一個檔是常見做法。
 *
 * 所以「多種可選」撐不住,得自帶素材。
 *
 * ── 自帶的四種音色從哪裡來 ──────────────────────────────────────────────
 * `scripts/gen_key_sounds.py` 合出來的,不是從外面找的。理由是這個專案的
 * 既有紀律:任何人都能重跑那支腳本、比對 PCM 的 sha256,授權來源就是
 * 「我們自己」,不必為第三方素材維護 attribution,也不會有哪天授權被撤的
 * 問題。音檔不連網,與離線定位沒有衝突。
 *
 * ── ⚠ 我們自己查一次 `sound_effects_enabled` ────────────────────────────
 * 兩個理由:
 *   1. `SoundPool` **完全不受**系統的按鍵音開關管 —— 那是我們自己的
 *      AudioTrack。使用者在系統設定裡關掉「按鍵音效」之後,自帶音色若照
 *      響不誤,就是震動那個 `FLAG_IGNORE_GLOBAL_SETTING` 的同一個病換一個
 *      形式。
 *   2. `playSoundEffect(fx, volume)` 這個**帶音量的多載**很可能繞過
 *      `querySoundEffectsEnabled()`(只有單參數版會查)。這一條我沒有實測 ——
 *      模擬器聽不到、dumpsys 也沒有逐次播放紀錄。與其賭 framework 的行為,
 *      不如自己查一次:查了之後兩條路的行為一致,而且不必依賴那個假設。
 */
object KeySounds {

    private const val TAG = "LuminaKeyFeel"

    /** 同時可能疊在一起的按鍵音:快打時前一顆還沒放完。4 條夠,再多是浪費。 */
    private const val MAX_STREAMS = 4

    private var pool: SoundPool? = null
    private var users = 0

    /** 資源名 → SoundPool 的 sampleId。載完才會進來(`load` 是非同步的)。 */
    private val loaded = HashMap<String, Int>()

    /**
     * 開始使用按鍵音。IME 的 `onCreate` 與設定頁的 `DisposableEffect` 各叫一次。
     *
     * 用引用計數而不是「IME 起來時載、`onDestroy` 釋放」:設定頁與輸入法在
     * **同一個行程**裡,照後者做的話,使用者一邊在設定頁試聽、輸入法剛好被
     * 系統收掉,試聽就會靜靜地變成沒有聲音。
     */
    @Synchronized
    fun acquire(context: Context) {
        users++
        if (pool != null) return
        val p = SoundPool.Builder()
            .setMaxStreams(MAX_STREAMS)
            .setAudioAttributes(
                AudioAttributes.Builder()
                    // 不是音樂、不是通知 —— 是「介面在回應你」。這個 usage 讓
                    // 按鍵音跟著系統音量走,而且不會打斷正在播的音樂。
                    .setUsage(AudioAttributes.USAGE_ASSISTANCE_SONIFICATION)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build()
            )
            .build()
        pool = p
        for (timbre in FeedbackPlan.SAMPLE_TIMBRES) {
            for (role in KeyRole.entries) {
                val name = FeedbackPlan.assetName(timbre, role)
                val id = p.load(context.applicationContext, resOf(timbre, role), 1)
                loaded[name] = id
            }
        }
    }

    @Synchronized
    fun release() {
        users--
        if (users > 0) return
        users = 0
        pool?.release()
        pool = null
        loaded.clear()
    }

    /**
     * 使用者在系統設定裡有沒有關掉按鍵音效。關掉了就一聲都不出 ——
     * 包含我們自己的素材。見檔頭。
     */
    private fun systemSoundEffectsOn(context: Context): Boolean = try {
        Settings.System.getInt(
            context.contentResolver,
            Settings.System.SOUND_EFFECTS_ENABLED,
            1,
        ) != 0
    } catch (_: Exception) {
        true
    }

    fun play(context: Context, plan: FeedbackPlan.Sound) {
        if (plan is FeedbackPlan.Sound.Silent) return
        if (!systemSoundEffectsOn(context)) {
            if (BuildConfig.DEBUG) android.util.Log.d(TAG, "sound suppressed by system setting")
            return
        }
        when (plan) {
            is FeedbackPlan.Sound.Silent -> return

            is FeedbackPlan.Sound.System -> playSystem(context, plan.role, plan.volume)

            is FeedbackPlan.Sound.Sample -> {
                val p = pool
                val id = loaded[plan.asset]
                // ⚠ `SoundPool.load` 是非同步的:剛啟動的頭幾百毫秒內 sampleId
                //   還沒準備好,`play` 會回 0 表示沒播成。那一刻**必須退回系統
                //   音效**,不能吞掉 —— 使用者按了鍵卻沒有聲音,他只會覺得
                //   「按鍵音壞了」。
                val stream = if (p != null && id != null && id != 0) {
                    p.play(id, plan.volume, plan.volume, 1, 0, 1f)
                } else {
                    0
                }
                if (stream == 0) {
                    playSystem(context, plan.role, plan.volume)
                    if (BuildConfig.DEBUG) {
                        android.util.Log.d(TAG, "sound sample=${plan.asset} not ready → system")
                    }
                } else if (BuildConfig.DEBUG) {
                    android.util.Log.d(
                        TAG,
                        "sound sample=${plan.asset} id=$id vol=${plan.volume}",
                    )
                }
            }
        }
    }

    private fun playSystem(context: Context, role: KeyRole, volume: Float) {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager ?: return
        val fx = fxOf(role)
        am.playSoundEffect(fx, volume.coerceIn(0f, 1f))
        if (BuildConfig.DEBUG) android.util.Log.d(TAG, "sound system fx=$fx vol=$volume")
    }

    /**
     * 角色 → 系統音效常數。
     *
     * 改動前這裡**一律是** `FX_KEYPRESS_STANDARD`(見舊版 `KeyBehavior`
     * 第 64 行)—— 連系統免費給的四個角色都沒有用上。這一段是這一版裡
     * 唯一不花任何代價的改善。
     */
    fun fxOf(role: KeyRole): Int = when (role) {
        KeyRole.SPACE -> AudioManager.FX_KEYPRESS_SPACEBAR
        KeyRole.DELETE -> AudioManager.FX_KEYPRESS_DELETE
        KeyRole.RETURN -> AudioManager.FX_KEYPRESS_RETURN
        KeyRole.STANDARD -> AudioManager.FX_KEYPRESS_STANDARD
    }

    /**
     * 資源查表。刻意寫成兩層 `when` 而不是 `getIdentifier()`:
     * 列舉是窮盡的,少一份素材會**編譯不過**,而 `getIdentifier` 少一份只會
     * 在執行期安靜地回 0,然後那一顆鍵沒有聲音。
     */
    private fun resOf(timbre: SoundTimbre, role: KeyRole): Int = when (timbre) {
        SoundTimbre.SOFT -> when (role) {
            KeyRole.STANDARD -> R.raw.key_soft_standard
            KeyRole.SPACE -> R.raw.key_soft_space
            KeyRole.DELETE -> R.raw.key_soft_delete
            KeyRole.RETURN -> R.raw.key_soft_return
        }

        SoundTimbre.MECHANICAL -> when (role) {
            KeyRole.STANDARD -> R.raw.key_mechanical_standard
            KeyRole.SPACE -> R.raw.key_mechanical_space
            KeyRole.DELETE -> R.raw.key_mechanical_delete
            KeyRole.RETURN -> R.raw.key_mechanical_return
        }

        SoundTimbre.DROP -> when (role) {
            KeyRole.STANDARD -> R.raw.key_drop_standard
            KeyRole.SPACE -> R.raw.key_drop_space
            KeyRole.DELETE -> R.raw.key_drop_delete
            KeyRole.RETURN -> R.raw.key_drop_return
        }

        // SYSTEM 沒有素材 —— 它走 playSoundEffect。呼叫端(acquire / play)
        // 都不會把它送到這裡,但列舉必須窮盡。
        SoundTimbre.SYSTEM -> R.raw.key_soft_standard
    }
}
