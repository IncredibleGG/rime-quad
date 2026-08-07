package org.rimequad.ime.prefs

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import org.rimequad.ime.theme.Appearance

/**
 * 「選了 `-dark` 主題，畫面還是淺色」。
 *
 * [applyThemePrefs] 刻意偏離規範 §8.2 第 1 條：釘了主題但深淺色仍是「跟隨
 * 系統」時，會跳到主題的 `counterpart`。清單裡列的是**個別的檔案**
 * （`intl-ios-dark`、`intl-ios-light`…），於是白天點深色主題會被立刻換回淺色，
 * 使用者看到的是「我點了它沒反應」。
 *
 * 修法是在**選取的當下**把深淺意圖一併記下來。這裡測那一步。
 */
class ThemeSelectionTest {

    private val known = mapOf(
        "intl-ios-dark" to Appearance.DARK,
        "intl-ios-light" to Appearance.LIGHT,
        "sakura-dark" to Appearance.DARK,
    )

    private fun appearanceOf(id: String): Appearance? = known[id]

    @Test
    fun pickingADarkThemeAlsoMeansIWantDark() {
        val after = UserPrefs().withThemeSelection("intl-ios-dark", ::appearanceOf)
        assertEquals("intl-ios-dark", after.themeId)
        assertEquals(AppearanceMode.DARK, after.appearanceMode)
        // 白天也要留在深色 —— 這正是缺陷的重現條件。
        assertEquals(true, wantsDark(after, systemDark = false))
    }

    @Test
    fun pickingALightThemeMeansIWantLight() {
        val after = UserPrefs(appearanceMode = AppearanceMode.DARK)
            .withThemeSelection("intl-ios-light", ::appearanceOf)
        assertEquals(AppearanceMode.LIGHT, after.appearanceMode)
        assertEquals(false, wantsDark(after, systemDark = true))
    }

    /**
     * 深淺色是選主題時**順帶**設的，所以取消指定主題時要一起清掉；
     * 留著會變成一個沒人記得自己設過、卻擋著系統深淺切換的幽靈設定。
     */
    @Test
    fun clearingTheThemeAlsoClearsTheAppearanceItImplied() {
        val pinned = UserPrefs().withThemeSelection("sakura-dark", ::appearanceOf)
        val cleared = pinned.withThemeSelection(null, ::appearanceOf)
        assertNull(cleared.themeId)
        assertNull(cleared.appearanceMode)
        assertEquals(true, wantsDark(cleared, systemDark = true))
        assertEquals(false, wantsDark(cleared, systemDark = false))
    }

    /** 主題載不起來時只改 themeId：猜一個深淺方向比不猜更糟。 */
    @Test
    fun anUnreadableThemeLeavesTheAppearanceAlone() {
        val before = UserPrefs(appearanceMode = AppearanceMode.LIGHT)
        val after = before.withThemeSelection("broken-theme", ::appearanceOf)
        assertEquals("broken-theme", after.themeId)
        assertEquals(AppearanceMode.LIGHT, after.appearanceMode)
    }

    /** 使用者選完主題之後仍然撥得動深淺色，而且撥完不會被蓋回去。 */
    @Test
    fun theUserCanStillAskToFollowTheSystemAfterwards() {
        val after = UserPrefs()
            .withThemeSelection("sakura-dark", ::appearanceOf)
            .copy(appearanceMode = AppearanceMode.FOLLOW_SYSTEM)
        assertEquals(false, wantsDark(after, systemDark = false))
        assertEquals(true, wantsDark(after, systemDark = true))
    }
}
