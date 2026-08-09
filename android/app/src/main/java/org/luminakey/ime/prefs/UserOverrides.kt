package org.luminakey.ime.prefs

import org.luminakey.ime.theme.HintPosition
import org.luminakey.ime.theme.Theme

/**
 * 三層覆寫的最後一層。
 *
 * ```
 * 主題 YAML 的值
 *   → platform_overrides.android      （規範 §7.4 第 5 步，由 ThemeParser 處理）
 *   → 使用者偏好                       （本檔，最高優先）
 * ```
 *
 * ── 為什麼是套在物件上而不是寫回 YAML ───────────────────────────────────
 * `core/themes` 下的主題檔 隨 APK 散佈、可能被主題市集更新、使用者也隨時可能
 * 換一份主題。把偏好寫回檔案會讓這三件事任何一件發生時都產生無法解釋的
 * 結果（「我明明沒改過為什麼字變小了」）。所以覆寫發生在**解析完成之後**
 * 的不可變 [Theme] 物件上，原始檔案永遠是乾淨的。
 *
 * ── 為什麼是純函式 ──────────────────────────────────────────────────────
 * 這一層的正確性只有一個判準：「未設定 → 主題值、設定 → 覆寫值、
 * 回復預設 → 主題值」。把它做成不碰 Android、不碰 Compose、不碰檔案的
 * 純函式，這個判準才寫得成單元測試（見 UserOverridesTest）。
 *
 * ── 不在這裡處理的東西 ──────────────────────────────────────────────────
 * [UserPrefs.themeId] 與 [UserPrefs.appearanceMode] 決定的是「載哪一份主題」，
 * 不是「主題裡的哪個值要換掉」，所以由 [org.luminakey.ime.keyboard.LayoutHost]
 * 消費。同理 [UserPrefs.simplification] / [UserPrefs.asciiPunct] 是 librime
 * 的 session 選項，走 `rs_set_option`。
 */
fun applyUserOverrides(theme: Theme, prefs: UserPrefs): Theme {
    if (prefs.isPristine) return theme
    var out = theme
    out = applyKeyboardHeight(out, prefs.keyboardHeightScale)
    out = applyHints(out, prefs.hints)
    out = applyCandidateSize(out, prefs.candidateSizeScale)
    out = applyCandidateCount(out, prefs.candidateCount)
    out = applyFeedback(out, prefs)
    return out
}

/* ────────────────────────────── 鍵盤高度 ────────────────────────────── */

/** 高度倍率的可用範圍。低於 0.6 鍵盤細到按不到，高於 1.5 會蓋掉半個螢幕。 */
const val HEIGHT_SCALE_MIN = 0.6f
const val HEIGHT_SCALE_MAX = 1.5f

/**
 * §8.8.0 的高度模型是**參考格 → 高度預算 → 列高**：鍵盤總高是算出來的結果，
 * 不是設定值。所以「鍵盤高度」這個偏好實際上要動的是**預算**。
 *
 * `key_aspect` 與 `key_height` 的上下界三個欄位一起乘上倍率，因為預算的公式是
 * `clamp(ref_key_w * key_aspect, key_height.min, key_height.max) * ref_rows`：
 *
 * 只乘 `key_aspect` 是錯的 —— 預設值 1.34 搭配 42–50dp 的夾制，在多數手機上
 * 本來就撞在上界，倍率會被夾制整個吃掉，使用者把滑桿拉到底只看到一點點變化。
 * 三個一起乘，預算才會嚴格等於倍率乘上原值。
 *
 * `row_height` 的上下界也要跟著乘。它們是**預算除完之後**的可用性護欄；
 * 不跟著縮，使用者把鍵盤調矮到某個點就會被下界擋住、總高固定的性質也一起破功。
 *
 * 安全網 `max_screen_ratio` 只在**放大**時跟著放寬（`max(s, 1)`）：
 * 使用者要更高的鍵盤時不該被安全網默默擋回去；但要更矮時把安全網一起收緊
 * 毫無意義，只會讓列數多的佈局（注音大千 5 列）莫名其妙被反推壓扁。
 *
 * 乘完一律夾回 §8.8 表列的合法值域，覆寫層沒有理由產生一個主題檔寫不出來的值。
 */
private fun applyKeyboardHeight(theme: Theme, scaleOrNull: Float?): Theme {
    val scale = scaleOrNull ?: return theme
    val s = scale.coerceIn(HEIGHT_SCALE_MIN, HEIGHT_SCALE_MAX)
    if (s == 1.0f) return theme
    val g = theme.keyboard.geometry
    val loosen = if (s > 1f) s else 1f
    return theme.copy(
        keyboard = theme.keyboard.copy(
            geometry = g.copy(
                aspect = (g.aspect * s).coerceIn(0.6f, 2.5f),
                keyHeightMin = (g.keyHeightMin * s).coerceIn(20f, 200f),
                keyHeightMax = (g.keyHeightMax * s).coerceIn(20f, 200f),
                rowHeightMin = (g.rowHeightMin * s).coerceIn(16f, 200f),
                rowHeightMax = (g.rowHeightMax * s).coerceIn(16f, 200f),
                maxScreenRatioPortrait =
                    (g.maxScreenRatioPortrait * loosen).coerceIn(0.2f, 0.8f),
                maxScreenRatioLandscape =
                    (g.maxScreenRatioLandscape * loosen).coerceIn(0.2f, 0.9f),
            )
        )
    )
}

/* ────────────────────────────── 鍵面 hint ────────────────────────────── */

/**
 * hint 是 per-key-style 的（§8.8.1 的 `hint_position`），所以覆寫必須逐個
 * style 套。三態的語義：
 *
 * · `null` / [HintVisibility.THEME] → 原封不動
 * · [HintVisibility.HIDDEN]         → 全部改成 `none`
 * · [HintVisibility.SHOWN]          → 只把寫成 `none` 的改成 `top_right`；
 *   已經有位置的 style 保留主題作者選的位置（使用者要的是「看得到」，
 *   不是「全部擠到右上角」）
 */
private fun applyHints(theme: Theme, mode: HintVisibility?): Theme {
    if (mode == null || mode == HintVisibility.THEME) return theme
    val styles = theme.keyboard.keyStyles.mapValues { (_, st) ->
        when (mode) {
            HintVisibility.HIDDEN -> st.copy(hintPosition = HintPosition.NONE)
            HintVisibility.SHOWN ->
                if (st.hintPosition == HintPosition.NONE) {
                    st.copy(hintPosition = HintPosition.TOP_RIGHT)
                } else {
                    st
                }

            HintVisibility.THEME -> st
        }
    }
    return theme.copy(keyboard = theme.keyboard.copy(keyStyles = styles))
}

/* ────────────────────────────── 候選列 ────────────────────────────── */

const val CANDIDATE_SIZE_SCALE_MIN = 0.75f
const val CANDIDATE_SIZE_SCALE_MAX = 1.6f

/**
 * 候選列字級。三個文字尺寸（序號 label、候選字 text、註解 comment）與
 * 候選列高度一起放大，否則字放大之後會被 44dp 的列高切掉上下緣。
 *
 * 列高夾回 §8.6.6 規定的 24–96dp。
 */
private fun applyCandidateSize(theme: Theme, scaleOrNull: Float?): Theme {
    val scale = scaleOrNull ?: return theme
    val s = scale.coerceIn(CANDIDATE_SIZE_SCALE_MIN, CANDIDATE_SIZE_SCALE_MAX)
    if (s == 1.0f) return theme
    val bar = theme.candidates.bar
    val st = bar.style
    return theme.copy(
        candidates = theme.candidates.copy(
            bar = bar.copy(
                height = (bar.height * s).coerceIn(24f, 96f),
                style = st.copy(
                    label = st.label.copy(size = st.label.size * s),
                    text = st.text.copy(size = st.text.size * s),
                    comment = st.comment.copy(size = st.comment.size * s),
                ),
            )
        )
    )
}

/** 對應 §8.6.6 的 `max_visible`（0 = 有多少畫多少）。 */
private fun applyCandidateCount(theme: Theme, countOrNull: Int?): Theme {
    val count = countOrNull ?: return theme
    val bar = theme.candidates.bar
    return theme.copy(
        candidates = theme.candidates.copy(
            bar = bar.copy(maxVisible = count.coerceIn(0, 30))
        )
    )
}

/* ────────────────────────────── 回饋 ────────────────────────────── */

/**
 * §8.10 的 `feedback`。規範自己在該節註記「觸覺回饋在概念上是行為不是外觀，
 * 放在主題檔裡並不乾淨……若日後拆出 `preferences`，這個區塊會被標記
 * deprecated」—— 使用者偏好正是那個 `preferences`，所以這裡覆寫得理直氣壯。
 */
private fun applyFeedback(theme: Theme, prefs: UserPrefs): Theme {
    val f = theme.feedback
    val next = f.copy(
        haptic = prefs.hapticEnabled ?: f.haptic,
        hapticStrength = prefs.hapticStrength ?: f.hapticStrength,
        sound = prefs.soundEnabled ?: f.sound,
        soundVolume = (prefs.soundVolume ?: f.soundVolume).coerceIn(0f, 1f),
    )
    return if (next == f) theme else theme.copy(feedback = next)
}
