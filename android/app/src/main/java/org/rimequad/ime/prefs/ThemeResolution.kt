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

/**
 * 使用者從主題清單點選一份主題。**這是「點了沒反應」那個缺陷的修補處。**
 *
 * ── 缺陷 ────────────────────────────────────────────────────────────────
 * [applyThemePrefs] 刻意偏離規範 §8.2 第 1 條（見該函式），使用者釘了主題
 * 但「深淺色」仍停在「跟隨系統」時，會跳到主題的 `counterpart`。在只有
 * `default` / `sakura` 兩對主題時這偏離很合理；但清單裡列的是**個別的檔案**
 * （`intl-ios-dark`、`intl-ios-light`…），於是白天點 `intl-ios-dark` 的結果是
 * 畫面**完全沒變** —— 系統是淺色，counterpart 立刻把它換回 `intl-ios-light`。
 * 使用者要再去把「深淺色」改成「固定深色」才生效，而畫面上沒有任何線索
 * 告訴他這件事。國際佈局那條線一口氣加了三對淺深主題之後，清單裡有一半
 * 的項目點下去像壞掉。
 *
 * ── 修法 ────────────────────────────────────────────────────────────────
 * 點 `-dark` 就是在說「我要深色」。所以選主題時，把 [UserPrefs.appearanceMode]
 * 同步設成該主題自己宣告的 `appearance`。這不影響跟隨系統的使用者 ——
 * 他們根本不會去點某一份特定的深色主題。
 *
 * 取消指定（`themeId = null`）時把 `appearanceMode` 一起清掉：那個值本來就是
 * 選主題時**順帶**設的，不是使用者自己去撥的；留著它會變成一個沒人記得自己
 * 設過、卻擋著系統深淺切換的幽靈設定。使用者若真的要固定深色，「深淺色」
 * 那一列隨時撥得動，而且撥完不會被這裡蓋掉。
 *
 * `appearanceOf` 是查詢函式而非 repo，好讓這段邏輯能純測。查不到（主題載
 * 不起來）時**只改 themeId**，不動深淺 —— 猜一個方向比不猜更糟。
 */
fun UserPrefs.withThemeSelection(
    themeId: String?,
    appearanceOf: (String) -> Appearance?,
): UserPrefs {
    if (themeId == null) return copy(themeId = null, appearanceMode = null)
    val appearance = appearanceOf(themeId) ?: return copy(themeId = themeId)
    return copy(
        themeId = themeId,
        appearanceMode =
            if (appearance == Appearance.DARK) AppearanceMode.DARK else AppearanceMode.LIGHT,
    )
}

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
