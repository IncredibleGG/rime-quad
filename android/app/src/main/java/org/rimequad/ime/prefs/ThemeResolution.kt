package org.rimequad.ime.prefs

import org.rimequad.ime.keyboard.ConfigRepository
import org.rimequad.ime.keyboard.LayoutHost
import org.rimequad.ime.theme.Appearance
import org.rimequad.ime.theme.Theme

/**
 * 「載哪一份主題」的偏好接線。
 *
 * 與 [applyUserOverrides] 分開的理由：那一支改的是**主題物件裡的值**，
 * 這一支決定的是**先載哪一份主題**。兩件事的失敗模式完全不同（前者最壞
 * 是值不對，後者最壞是畫不出鍵盤），混在一起會讓「主題載不起來」的退路
 * 沒地方寫。
 */

/** [UserPrefs.appearanceMode] + 系統外觀 → 這一刻想要深色嗎。 */
fun wantsDark(prefs: UserPrefs, systemDark: Boolean): Boolean =
    when (prefs.appearanceMode ?: AppearanceMode.FOLLOW_SYSTEM) {
        AppearanceMode.FOLLOW_SYSTEM -> systemDark
        AppearanceMode.LIGHT -> false
        AppearanceMode.DARK -> true
    }

/**
 * 依偏好驅動 [LayoutHost] 選出當前主題。
 *
 * 規範 §8.2 的執行期規則第 1 條是「使用者明確指定了某個主題 id → 直接用它，
 * **不做**深淺切換」。這裡刻意偏離一格：使用者指定主題 id **且**同時要求
 * 「跟隨系統」時，仍會跳到 `counterpart`。
 *
 * 理由：規範寫那一條時假設「選主題」與「選深淺」是同一個動作。但設定畫面
 * 把它們拆成兩個控制項之後，使用者選了「sakura」又選了「跟隨系統」，
 * 照字面實作的結果是系統轉深色而鍵盤仍是刺眼的淺色 —— 那不是任何人要的。
 * 已在回報中列為規範需要澄清之處。
 *
 * counterpart 載不起來時**還原成原本指定的 id**（而不是留在半途），
 * 否則 `pinnedThemeId` 會被一個載不起來的 id 卡住，之後每次都失敗。
 */
fun LayoutHost.applyThemePrefs(prefs: UserPrefs, systemDark: Boolean) {
    val wantDark = wantsDark(prefs, systemDark)
    val pinned = prefs.themeId
    if (pinned == null) {
        pinTheme(null)
        applyAppearance(wantDark)
        return
    }
    pinTheme(pinned)
    applyAppearance(wantDark)
    val current = theme ?: return
    if ((current.appearance == Appearance.DARK) == wantDark) return
    val counterpart = current.counterpart ?: return
    pinTheme(counterpart)
    applyAppearance(wantDark)
    if (theme?.id != counterpart) {
        // 沒切成功：還原，免得下次又從壞掉的 id 出發。
        pinTheme(pinned)
        applyAppearance(wantDark)
    }
}

/**
 * 設定畫面用：算出「若不套任何值覆寫，現在會是哪一份主題」。
 *
 * 設定畫面需要它來顯示每一項的**主題原值**（「預設 33%」之類），
 * 讓「回復預設」不是一個看不出效果的按鈕。
 */
fun resolveBaseTheme(repo: ConfigRepository, prefs: UserPrefs, systemDark: Boolean): Theme {
    val host = LayoutHost(repo)
    host.applyThemePrefs(prefs, systemDark)
    return host.theme ?: repo.builtinFallbackTheme()
}
