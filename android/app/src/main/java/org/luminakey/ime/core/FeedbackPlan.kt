package org.luminakey.ime.core

import org.luminakey.ime.theme.HapticStrength

/**
 * 按鍵音色。與音量是**兩個獨立的設定** —— 合成一個就會變成 8 個檔位
 * (`docs/ui-design.md` §4.2 說分段控制 2–4 格),而且「關」該算音量還是
 * 算音色會說不清。
 *
 * [SYSTEM] 不是「沒有音色」,是「照這支手機的 OEM 調校走」——
 * `AudioManager.playSoundEffect`。它是預設值,因為各家 ROM 的按鍵音
 * 幾乎都比我們合成的好聽,而且與使用者其他 app 的手感一致。
 */
enum class SoundTimbre { SYSTEM, SOFT, MECHANICAL, DROP }

/**
 * 這一顆鍵在「按鍵音」這件事上算哪一種。
 *
 * 系統本來就把按鍵音分成這四個角色(`FX_KEYPRESS_STANDARD` / `SPACEBAR` /
 * `DELETE` / `RETURN`),而在這一版之前我們**一律送 STANDARD** ——
 * 連系統免費給的區分都沒有用上。自帶音色沒有理由做得更粗,所以四種角色
 * 各有一份素材。
 */
enum class KeyRole { STANDARD, SPACE, DELETE, RETURN }

/**
 * 「使用者選了哪一階」→「要送出什麼」的**唯一一份**對照表。
 *
 * ── 為什麼要有這一層 ────────────────────────────────────────────────────
 * 這件事有四個消費端:鍵盤上的每一次按鍵、App 設定頁的試聽、鍵盤內快捷
 * 面板的試聽,以及日後的主題覆寫。四個地方各寫一份 `when`,遲早會出現
 * 「設定頁試聽是水滴、真的打字是系統音」這種缺陷 —— 而它在畫面上完全
 * 看不出來,只有耳朵分得出。
 *
 * 這裡不碰 Android:回傳的是**描述**(要震多久多強、要播哪一份素材),
 * 由 `prefs/KeyHaptics.kt` 與 `prefs/KeySounds.kt` 兩個薄薄的出口翻譯成
 * 平台呼叫。於是「選了之後送出什麼」整件事可以直接被 JVM 單元測試釘住。
 *
 * ── 震動為什麼從常數改成振幅 ────────────────────────────────────────────
 * 舊版走 `View.performHapticFeedback` 的三個常數(`CLOCK_TICK` /
 * `KEYBOARD_TAP` / `LONG_PRESS`)。在 API 35 的 AOSP 上實測 `dumpsys
 * vibrator_manager`,那三個常數播出來的是:
 *
 *     弱 → Prebaked=TEXTURE_TICK   101 ms
 *     中 → Prebaked=CLICK          101 ms
 *     強 → Prebaked=HEAVY_CLICK     30 ms   ← 比「中」短三倍
 *
 * 它們是三個不相干的波形,不是同一個波形的三種大小 —— 使用者要的「大小」
 * 在那個階梯上根本不存在。而且只有「中」帶 `category=KEYBOARD`,所以在
 * 三階之間切,順便換掉了「哪一個系統開關管得到我」。
 *
 * 要真的可調就得走 `Vibrator` + `VibrationEffect.createOneShot(ms, amplitude)`,
 * 那需要 `android.permission.VIBRATE`。舊註解說那是「執行期權限,為了三段
 * 強度去要划不來」—— **那句話是錯的**:VIBRATE 是 `normal` 等級,裝機自動
 * 授予,不跳對話框,使用者也取消不掉,而且它不是資料通道(沒有東西能透過
 * 馬達離開這台機器)。決定建立在一個假前提上。
 */
object FeedbackPlan {

    /* ─────────────────────────── 按鍵音 ─────────────────────────── */

    sealed interface Sound {
        /** 不出聲。使用者關掉了、或音量是 0。 */
        data object Silent : Sound

        /** 交給 OEM:`AudioManager.playSoundEffect(fxOf(role), volume)`。 */
        data class System(val role: KeyRole, val volume: Float) : Sound

        /** 自帶素材:`res/raw/[asset].ogg`,由 `SoundPool` 播。 */
        data class Sample(
            val timbre: SoundTimbre,
            val role: KeyRole,
            val volume: Float,
        ) : Sound {
            val asset: String get() = assetName(timbre, role)
        }
    }

    /**
     * @param enabled 使用者的「按鍵音」開關(已含主題回落)
     * @param volume  0f–1f
     * @param timbre  音色
     * @param role    這一顆鍵算哪一種
     */
    fun sound(
        enabled: Boolean,
        volume: Float,
        timbre: SoundTimbre,
        role: KeyRole,
    ): Sound {
        val v = volume.coerceIn(0f, 1f)
        // 音量 0 與「關掉」在這一層是同一件事。分開處理只會多一條沒有人
        // 走得到的分支,以及一次「音量拉到 0 卻還是有聲音」的迴歸。
        if (!enabled || v <= 0f) return Sound.Silent
        return when (timbre) {
            SoundTimbre.SYSTEM -> Sound.System(role, v)
            else -> Sound.Sample(timbre, role, v)
        }
    }

    /**
     * `res/raw/` 底下的資源名。與 `scripts/gen_key_sounds.py` 的輸出檔名
     * 是同一個規則 —— 那支腳本產生的就是這 12 份。
     */
    fun assetName(timbre: SoundTimbre, role: KeyRole): String =
        "key_" + timbre.name.lowercase() + "_" + role.name.lowercase()

    /** 自帶素材的完整清單。`KeySounds` 預載時照這一份走,不另外抄一份。 */
    val SAMPLE_TIMBRES: List<SoundTimbre> =
        SoundTimbre.entries.filter { it != SoundTimbre.SYSTEM }

    /* ─────────────────────────── 震動 ─────────────────────────── */

    sealed interface Haptic {
        data object Silent : Haptic

        /** `VibrationEffect.createOneShot(durationMs, amplitude)`。 */
        data class OneShot(val durationMs: Int, val amplitude: Int) : Haptic

        /**
         * 這支手機的馬達沒有 `hasAmplitudeControl()` —— 退回舊的常數。
         *
         * ⚠ 這是**誠實的降級**:設定頁必須把「這支手機分不出強弱」講出來,
         *    不可以無聲地讓三階變成同一種感覺。
         */
        data class Constant(val kind: HapticConstant) : Haptic
    }

    /** 退回路徑用得到的 `HapticFeedbackConstants`,包一層免得純函式碰 Android。 */
    enum class HapticConstant { CLOCK_TICK, KEYBOARD_TAP, LONG_PRESS }

    /**
     * 振幅階梯。0–255,系統之後還會再乘上使用者的觸覺強度
     * (`VibrationScaler` 的 0.6 / 0.8 / 1.0 / 1.2 / 1.4)—— 我們活在那個
     * 縮放**之內**,不疊在上面。
     */
    private val AMPLITUDE = mapOf(
        HapticStrength.LIGHT to 60,
        HapticStrength.MEDIUM to 130,
        HapticStrength.HEAVY to 220,
    )

    /**
     * 時長。**15–25 ms,不是 100 ms。**
     *
     * 理由是量到的:`dumpsys vibrator_manager` 在快打時出現大量
     * `cancelled_superseded` —— 100 ms 的波形還沒播完,下一顆鍵就把它砍了。
     * 一顆按鍵的觸覺回饋只需要「有東西碰了我一下」,不需要一段旋律。
     */
    private val DURATION_MS = mapOf(
        HapticStrength.LIGHT to 15,
        HapticStrength.MEDIUM to 20,
        HapticStrength.HEAVY to 25,
    )

    /**
     * @param enabled           使用者的「震動」開關(已含主題回落)
     * @param strength          四階裡的哪一階
     * @param amplitudeControl  `Vibrator.hasAmplitudeControl()`
     */
    fun haptic(
        enabled: Boolean,
        strength: HapticStrength,
        amplitudeControl: Boolean,
    ): Haptic {
        if (!enabled || strength == HapticStrength.NONE) return Haptic.Silent
        if (!amplitudeControl) {
            return Haptic.Constant(
                when (strength) {
                    HapticStrength.LIGHT -> HapticConstant.CLOCK_TICK
                    HapticStrength.HEAVY -> HapticConstant.LONG_PRESS
                    else -> HapticConstant.KEYBOARD_TAP
                }
            )
        }
        return Haptic.OneShot(
            durationMs = DURATION_MS.getValue(strength),
            amplitude = AMPLITUDE.getValue(strength),
        )
    }

    /* ─────────────────────────── 角色判定 ─────────────────────────── */

    /**
     * 這一顆鍵算哪一個角色。
     *
     * 先看 keysym(那是語意來源,四份佈局都一樣),認不得再看 `id`。
     * 兩個都認不得就是一般鍵 —— 這一層不該有「猜錯就沒有聲音」的可能。
     *
     * ⚠ 只認 keysym 不夠:九宮格的空白鍵在某些佈局裡送的是 `KP_Space`,
     *   而確認鍵在搜尋框情境下是 `KP_Enter`。所以兩種名字都收。
     */
    fun roleOf(keysym: String? = null, id: String? = null): KeyRole {
        // ⚠ keysym 存在時就**只看 keysym**,不再退而看 id。
        //
        //   兩者都看的寫法有一個安靜的錯:把 id 取名 "space" 的普通鍵判成
        //   空白鍵,於是那一顆鍵的聲音與旁邊的不一樣 —— 沒有人會回報這種事,
        //   但它就是不對。id 是給主題與換鍵用的名字,人可以隨便取;keysym 是
        //   真的送進引擎的東西。有後者就相信後者。
        val k = keysym?.trim()?.lowercase()
        if (!k.isNullOrEmpty()) {
            return when (k) {
                "space", "kp_space" -> KeyRole.SPACE
                "backspace", "delete", "kp_delete" -> KeyRole.DELETE
                "return", "kp_enter", "linefeed" -> KeyRole.RETURN
                else -> KeyRole.STANDARD
            }
        }
        // 沒有 keysym 的鍵(送 text、或只有 tap 動作)才輪到 id。
        return when (id?.trim()?.lowercase()) {
            "space" -> KeyRole.SPACE
            "backspace", "delete" -> KeyRole.DELETE
            "enter", "return" -> KeyRole.RETURN
            else -> KeyRole.STANDARD
        }
    }
}
