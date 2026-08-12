package org.luminakey.ime.prefs

import org.luminakey.ime.core.SoundTimbre
import org.luminakey.ime.theme.HapticStrength

/**
 * 使用者偏好 —— 三層覆寫的最上層。
 *
 * ── 為什麼每一個欄位都是 nullable ────────────────────────────────────────
 * `null` 的語義是「**使用者沒有設定過這一項**」，而不是「使用者選了預設值」。
 * 這個區別是本檔最重要的設計決定：
 *
 *   · 存副本的作法（把主題目前的值抄一份進偏好）會讓「回復預設」變成
 *     「回到當初抄下來的那個值」。主題檔更新之後，使用者永遠停在舊值上，
 *     而且他無從得知為什麼別人的鍵盤變好看了他的沒有。
 *   · 存 null 的作法讓「回復預設」等於「把這個 key 從儲存層刪掉」，
 *     下一次解析主題檔取到什麼就是什麼。
 *
 * 因此 [toMap] **絕不**為 null 欄位寫入任何條目，[fromMap] 遇到缺席的
 * key 也**絕不**填入預設值。這一對函式是純的，由 UserPrefsTest 直接驗證。
 *
 * ── 為什麼多半是「倍率」而不是絕對值 ────────────────────────────────────
 * 鍵盤高度、候選字級這類設定若存絕對值（例如 240dp、18sp），同樣會有
 * 「主題更新了但使用者被釘在舊數字上」的問題，而且換主題之後那個絕對值
 * 通常不再合適。存倍率則是「相對於當前主題」的意圖，換主題、更新主題
 * 都仍然說得通。
 *
 * ── 與主題格式的關係 ────────────────────────────────────────────────────
 * 這一層**不寫回** `core/themes` 下的主題檔。那些檔案隨 APK 散佈，而且使用者
 * 隨時可能換主題。覆寫發生在**已解析完成**的 [org.luminakey.ime.theme.Theme]
 * 物件之上，見 [applyUserOverrides]。
 */
data class UserPrefs(

    /* ─────────────── 鍵盤 ─────────────── */

    /** 鍵盤高度倍率，套在主題 `keyboard.height` 的 ratio/min/max 上。 */
    val keyboardHeightScale: Float? = null,

    /** 對應主題 `feedback.sound`。 */
    val soundEnabled: Boolean? = null,

    /** 對應主題 `feedback.sound_volume`。 */
    val soundVolume: Float? = null,

    /**
     * 按鍵音的**音色**。`null` = 未設定 = [org.luminakey.ime.core.SoundTimbre.SYSTEM]
     * (照這支手機 OEM 的調校走)。
     *
     * ⚠ 這一項**沒有主題欄位可退**。`docs/theme-format.md` §8.10 只有
     *   `feedback.sound` 與 `feedback.sound_volume`,沒有音色 —— 而那份文件的
     *   擁有者是 macOS 端(`docs/coordination.md` §2 的檔案所有權表),
     *   Android 不能自己加,只能提出。已寫進 §5。
     *
     *   在它被加進規範之前,這裡的 null 退回的是本專案的預設值 SYSTEM,
     *   不是主題值。與 [longPressMs] 那三項同一種誠實的降級。
     */
    val soundTimbre: SoundTimbre? = null,

    /** 對應主題 `feedback.haptic`。 */
    val hapticEnabled: Boolean? = null,

    /** 對應主題 `feedback.haptic_strength`。 */
    val hapticStrength: HapticStrength? = null,

    /**
     * 長按觸發延遲（毫秒）。
     * ⚠ 規範缺口：主題格式沒有這個欄位，`null` 只能退回 [KeyBehavior.DEFAULT]。
     */
    val longPressMs: Int? = null,

    /** 按住到開始重複的延遲（毫秒）。⚠ 同上，規範缺口。 */
    val repeatDelayMs: Int? = null,

    /** 重複送出的間隔（毫秒）。⚠ 同上，規範缺口。 */
    val repeatIntervalMs: Int? = null,

    /** 鍵面角落小字（拼音佈局上的數字提示）。對應主題各 key_style 的 `hint_position`。 */
    val hints: HintVisibility? = null,

    /* ─────────────── 外觀 ─────────────── */

    /**
     * 使用者選定的主題 id；`null` = 用內建預設（`default-light` / `default-dark`）。
     * 這一項**不是**主題欄位的覆寫，而是「載哪一份主題」，因此由
     * [org.luminakey.ime.keyboard.LayoutHost] 消費而非 [applyUserOverrides]。
     */
    val themeId: String? = null,

    /** 深淺色策略。`null` = 跟隨系統（§8.2 執行期規則第 2 條）。 */
    val appearanceMode: AppearanceMode? = null,

    /**
     * 使用者在鍵盤類型選單裡**明確挑過**的佈局，`schema id → layout id`。
     *
     * 規範 §9.1.1 的 SHOULD：「使用者若曾為當前 schema 明確指定過佈局，
     * 實作**應**記住該選擇並跳過第 1 步」。少了持久化，這條 SHOULD 只在
     * 本次輸入法行程內成立 —— 使用者換個 app、或系統回收了 IME 之後，
     * 他挑的九宮格就悄悄變回全鍵盤。
     *
     * 存成單一字串而不是拆成多個 key：DataStore 的 key 是靜態宣告的，
     * 「每個方案一個 key」需要動態 key，而且刪方案時會留下孤兒。
     * 編解碼見 [encodeLayoutPins] / [decodeLayoutPins]。
     */
    val layoutPins: String? = null,

    /** 候選列字級倍率，套在 `candidates.bar` 的 label/text/comment size 與 bar 高度上。 */
    val candidateSizeScale: Float? = null,

    /* ─────────────── 輸入 ─────────────── */

    /** 候選列最多顯示幾個；對應主題 `candidates.bar.max_visible`（0 = 不限）。 */
    val candidateCount: Int? = null,

    /** librime 的 `simplification` 開關。`null` = 不干預，交給方案自己的預設。 */
    val simplification: Boolean? = null,

    /** librime 的 `ascii_punct` 開關。`null` = 不干預。 */
    val asciiPunct: Boolean? = null,

    /**
     * 字集守門：選了簡體就只留簡體字、選了繁體就只留繁體字。
     *
     * ⚠ 這一項的 `null` 與本檔其他欄位不同：**未設定 = 開**。
     * 理由與 [networkEnabled] 對稱 —— 那一項的安全預設是「不連網」，
     * 這一項的產品預設是「使用者說要簡體就給簡體」。消費端一律寫
     * `charsetGuard != false`，不可以寫 `charsetGuard == true`。
     *
     * 送給引擎的是**否定式**的 `luminakey_charset_off`：librime 沒有
     * 「宣告過但預設為真」的 option，沒宣告的一律讀成 false，所以
     * 「預設開」只能寫成「預設沒有關」。轉換在 applyRimeOptions()。
     *
     * 代價寫在設定頁的說明裡，不可以只寫在這裡：字集之外還有字集，
     * 罕用字（蚵 砦 疋 糸 酶 礴 珏）會一起被濾掉。見
     * `docs/settings-model.md` §4.7。
     */
    val charsetGuard: Boolean? = null,

    /** 空白鍵行為。⚠ 規範缺口：主題與佈局格式都沒有描述這件事。 */
    val spaceBehavior: SpaceBehavior? = null,

    /* ─────────────── 連網 ─────────────── */

    /**
     * 連網總開關。`null` = 未設定，行為上等同**關**。
     *
     * 這一項與本檔其他欄位有一個關鍵差別：其他欄位的「未設定」是為了讓
     * 主題檔日後更新時使用者跟著更新；這一項的「未設定」則是一個安全預設 ——
     * **從沒表態過的人一律當作不連網**。消費端一律寫 `networkEnabled == true`，
     * 不可以寫 `networkEnabled ?: true`。
     *
     * 實際的閘門在 [org.luminakey.ime.net.NetworkGate]，這裡只是它的儲存。
     */
    val networkEnabled: Boolean? = null,

    /**
     * 首次啟動的離線說明看過了。`null` = 還沒看過。
     *
     * 為什麼放在偏好而不是另開一個 SharedPreferences：少一個儲存層就少一個
     * 要解釋的東西。副作用是「全部回復預設」會讓說明再出現一次 —— 那是
     * 對的，因為同一次回復也把連網開關關回去了，兩件事該一起重來。
     */
    val offlineNoticeSeen: Boolean? = null,

    /* ─────────────── 引導 ─────────────── */

    /**
     * 曾經走完一次首次啟動的引導。`null` = 還沒有。
     *
     * ⚠ **語義必須釘死**：它**只**決定 app 冷啟時預設落在哪一頁；
     * 它**不是**「輸入法可以用」的證據。
     *
     * 因為使用者完全可能在引導完成之後，到系統設定裡把鍵盤換回別的。那時
     * app 應該開在設定（他已經是老手了），但首頁的狀態卡片必須誠實地顯示
     * 「現在打字用的不是這個鍵盤」並給一顆一鍵切回去的按鈕。把兩件事綁在
     * 同一個布林上，就會做出一個「明明不能用卻說一切正常」的 app。
     *
     * 寫入時機是**第一次觀察到系統說我們已經是預設輸入法**，而不是「使用者
     * 按了完成」—— 一律以系統事實為準（見 home/Onboarding.kt）。
     *
     * ⚠ 消費端必須**阻塞讀**這一個 key（`PrefsStore.current`），不能等
     * DataStore 的非同步首讀，否則既有使用者每次冷啟動都會看到引導頁閃一下。
     */
    val onboardingDone: Boolean? = null,

    /* ─────────────── 更新 ─────────────── */

    /**
     * 啟動時自動檢查更新。`null` = 未設定，行為上等同**開**。
     *
     * 為什麼「未設定」不寫成 `true`：這一檔的第一原則就是「不把預設值抄一份
     * 進偏好」。日後若判定預設該改成關（例如改用系統的排程檢查），
     * 存了 `true` 的使用者會被永遠釘在舊預設上，而且他從沒動過這一項。
     * 消費端一律寫 `autoCheckUpdate ?: true`。
     */
    val autoCheckUpdate: Boolean? = null,
) {

    /** true = 使用者一項都沒設定過，全部走主題檔的值。 */
    val isPristine: Boolean get() = this == UserPrefs()

    /**
     * 序列化成「只含使用者真的設過的 key」的映射。
     *
     * 儲存層（[PrefsStore]）拿這份映射去寫 DataStore：出現在映射裡的 key 才寫，
     * 沒出現的 key 一律 **remove**。這就是「回復預設 = 刪 key」的兌現處。
     */
    fun toMap(): Map<String, Any> {
        val m = LinkedHashMap<String, Any>()
        keyboardHeightScale?.let { m[K_HEIGHT_SCALE] = it }
        soundEnabled?.let { m[K_SOUND] = it }
        soundVolume?.let { m[K_SOUND_VOLUME] = it }
        soundTimbre?.let { m[K_SOUND_TIMBRE] = it.name }
        hapticEnabled?.let { m[K_HAPTIC] = it }
        hapticStrength?.let { m[K_HAPTIC_STRENGTH] = it.name }
        longPressMs?.let { m[K_LONG_PRESS_MS] = it }
        repeatDelayMs?.let { m[K_REPEAT_DELAY_MS] = it }
        repeatIntervalMs?.let { m[K_REPEAT_INTERVAL_MS] = it }
        hints?.let { m[K_HINTS] = it.name }
        themeId?.let { m[K_THEME_ID] = it }
        appearanceMode?.let { m[K_APPEARANCE] = it.name }
        layoutPins?.let { m[K_LAYOUT_PINS] = it }
        candidateSizeScale?.let { m[K_CANDIDATE_SIZE] = it }
        candidateCount?.let { m[K_CANDIDATE_COUNT] = it }
        simplification?.let { m[K_SIMPLIFICATION] = it }
        asciiPunct?.let { m[K_ASCII_PUNCT] = it }
        charsetGuard?.let { m[K_CHARSET_GUARD] = it }
        spaceBehavior?.let { m[K_SPACE] = it.name }
        networkEnabled?.let { m[K_NETWORK_ENABLED] = it }
        offlineNoticeSeen?.let { m[K_OFFLINE_NOTICE_SEEN] = it }
        onboardingDone?.let { m[K_ONBOARDING_DONE] = it }
        autoCheckUpdate?.let { m[K_AUTO_CHECK_UPDATE] = it }
        return m
    }

    companion object {
        const val K_HEIGHT_SCALE = "keyboard_height_scale"
        const val K_SOUND = "sound_enabled"
        const val K_SOUND_VOLUME = "sound_volume"
        const val K_SOUND_TIMBRE = "sound_timbre"
        const val K_HAPTIC = "haptic_enabled"
        const val K_HAPTIC_STRENGTH = "haptic_strength"
        const val K_LONG_PRESS_MS = "long_press_ms"
        const val K_REPEAT_DELAY_MS = "repeat_delay_ms"
        const val K_REPEAT_INTERVAL_MS = "repeat_interval_ms"
        const val K_HINTS = "key_hints"
        const val K_THEME_ID = "theme_id"
        const val K_APPEARANCE = "appearance_mode"
        const val K_LAYOUT_PINS = "layout_pins"
        const val K_CANDIDATE_SIZE = "candidate_size_scale"
        const val K_CANDIDATE_COUNT = "candidate_count"
        const val K_SIMPLIFICATION = "opt_simplification"
        const val K_ASCII_PUNCT = "opt_ascii_punct"
        const val K_CHARSET_GUARD = "opt_charset_guard"
        const val K_SPACE = "space_behavior"
        const val K_NETWORK_ENABLED = "network_enabled"
        const val K_OFFLINE_NOTICE_SEEN = "offline_notice_seen"
        const val K_ONBOARDING_DONE = "onboarding_done"
        const val K_AUTO_CHECK_UPDATE = "auto_check_update"

        /**
         * 反序列化。**缺席的 key 一律留 null**，不得填入任何預設值 ——
         * 填了就等於把預設值複製一份進偏好，正是本檔開頭要避免的事。
         *
         * 值的型別不符或列舉名認不得時，視同未設定（丟掉壞資料，不讓
         * 一個壞掉的 key 把整份偏好拖垮）。
         */
        fun fromMap(m: Map<String, Any?>): UserPrefs = UserPrefs(
            keyboardHeightScale = m.float(K_HEIGHT_SCALE),
            soundEnabled = m.bool(K_SOUND),
            soundVolume = m.float(K_SOUND_VOLUME),
            soundTimbre = m.enum(K_SOUND_TIMBRE, SoundTimbre.entries),
            hapticEnabled = m.bool(K_HAPTIC),
            hapticStrength = m.enum(K_HAPTIC_STRENGTH, HapticStrength.entries),
            longPressMs = m.int(K_LONG_PRESS_MS),
            repeatDelayMs = m.int(K_REPEAT_DELAY_MS),
            repeatIntervalMs = m.int(K_REPEAT_INTERVAL_MS),
            hints = m.enum(K_HINTS, HintVisibility.entries),
            themeId = m[K_THEME_ID] as? String,
            appearanceMode = m.enum(K_APPEARANCE, AppearanceMode.entries),
            layoutPins = (m[K_LAYOUT_PINS] as? String)?.takeIf { it.isNotEmpty() },
            candidateSizeScale = m.float(K_CANDIDATE_SIZE),
            candidateCount = m.int(K_CANDIDATE_COUNT),
            simplification = m.bool(K_SIMPLIFICATION),
            asciiPunct = m.bool(K_ASCII_PUNCT),
            charsetGuard = m.bool(K_CHARSET_GUARD),
            spaceBehavior = m.enum(K_SPACE, SpaceBehavior.entries),
            networkEnabled = m.bool(K_NETWORK_ENABLED),
            offlineNoticeSeen = m.bool(K_OFFLINE_NOTICE_SEEN),
            onboardingDone = m.bool(K_ONBOARDING_DONE),
            autoCheckUpdate = m.bool(K_AUTO_CHECK_UPDATE),
        )

        /* ────────────── [layoutPins] 的編解碼 ────────────── */

        private const val PIN_SEP = ';'
        private const val PIN_EQ = '='

        /**
         * `schema=layout;schema=layout` —— 刻意用最笨的格式。
         *
         * schema id 與 layout id 都是檔名等級的識別字（`luna_pinyin`、
         * `cn-t9-pinyin`），不含 `;` 或 `=`。真的含了就**丟掉那一筆**：
         * 寧可少記一個偏好，也不要寫出一個解回來會錯位的字串。
         *
         * 空映射回 `null` 而不是空字串 —— 「使用者一項都沒指定過」在本檔的
         * 語義裡就是「這個 key 不存在」。
         */
        fun encodeLayoutPins(pins: Map<String, String>): String? {
            val safe = pins.entries.filter { (k, v) ->
                k.isNotEmpty() && v.isNotEmpty() &&
                    !k.contains(PIN_SEP) && !k.contains(PIN_EQ) &&
                    !v.contains(PIN_SEP) && !v.contains(PIN_EQ)
            }
            if (safe.isEmpty()) return null
            return safe.joinToString(PIN_SEP.toString()) { "${it.key}$PIN_EQ${it.value}" }
        }

        /** 解不開的片段一律略過，不讓一筆壞資料把整份指定弄丟。 */
        fun decodeLayoutPins(encoded: String?): Map<String, String> {
            if (encoded.isNullOrEmpty()) return emptyMap()
            val out = LinkedHashMap<String, String>()
            for (part in encoded.split(PIN_SEP)) {
                val i = part.indexOf(PIN_EQ)
                if (i <= 0 || i == part.length - 1) continue
                out[part.substring(0, i)] = part.substring(i + 1)
            }
            return out
        }

        private fun Map<String, Any?>.float(k: String): Float? = (this[k] as? Number)?.toFloat()
        private fun Map<String, Any?>.int(k: String): Int? = (this[k] as? Number)?.toInt()
        private fun Map<String, Any?>.bool(k: String): Boolean? = this[k] as? Boolean
        private fun <E : Enum<E>> Map<String, Any?>.enum(k: String, all: List<E>): E? {
            val name = this[k] as? String ?: return null
            return all.firstOrNull { it.name == name }
        }
    }
}

/** 深淺色策略（§8.2 的執行期外觀選擇）。 */
enum class AppearanceMode { FOLLOW_SYSTEM, LIGHT, DARK }

/**
 * 鍵面 hint 的三態。
 *
 * 刻意不是 `Boolean`：`null`（未設定）與 `THEME` 是同一件事，但列舉本身
 * 必須能表達「使用者主動要求跟隨主題」與「使用者主動要求關掉」的差別，
 * 否則設定畫面的第三個選項無處可存。
 */
enum class HintVisibility {
    /** 由主題各 key_style 的 `hint_position` 決定。 */
    THEME,

    /** 一律不顯示（拼音佈局上的數字提示對多數人是噪音）。 */
    HIDDEN,

    /** 一律顯示；主題寫 `none` 的 style 退回 `top_right`。 */
    SHOWN,
}

/**
 * 空白鍵行為。
 *
 * ⚠ 規範缺口：`docs/theme-format.md` 的主題與佈局格式都沒有描述
 * 「組字中按空白鍵要做什麼」，佈局只能寫 `send: { keysym: "space" }`。
 * 這個列舉因此是**實作層**的擴充，不是主題欄位的覆寫。
 */
enum class SpaceBehavior {
    /** 把 space 交給 librime，由方案自己決定（多數方案 = 選第一個候選）。 */
    SCHEMA_DEFAULT,

    /** 一律先把組字內容上屏，再送一個半形空白。 */
    ALWAYS_SPACE,
}
