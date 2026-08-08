package org.rimequad.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.rimequad.ime.theme.LayoutParser
import org.rimequad.ime.theme.Platform
import org.rimequad.ime.theme.RepoFixtures
import org.rimequad.ime.theme.ThemeLoader
import java.io.File

/**
 * TalkBack 念得出每一顆鍵,而且念完之後**按得動**。
 *
 * ── 為什麼後半句要單獨寫出來 ────────────────────────────────────────────
 * 補 contentDescription 是這件事最直覺、也最容易只做一半的做法。做一半的
 * 結果不是「無障礙做得不夠好」,是一顆**念得出名字、聚焦得到、輕點兩下什麼
 * 都不會發生**的鍵 —— 和這個專案抓過的重輸鍵、中英鍵、表情鍵是同一種東西,
 * 只是這一次只有用 TalkBack 的人碰得到,所以更不會有人回報。
 *
 * 原因是 [KeyView] 的觸發整段都在 `pointerInput` 裡,而 TalkBack 的「輕點兩下」
 * 送的是無障礙的 ACTION_CLICK,不會變成 pointer 事件。
 *
 * ── 它抓不到什麼(誠實說明)──────────────────────────────────────────────
 * 這裡驗的是**規則與接線**:名字算不算得出來、KeyView 有沒有把語意動作接上。
 * 它不會替你按一次 —— 「ACTION_CLICK 真的送到 fire() 了嗎」要 Compose UI 測試
 * 或真機開 TalkBack。那一層寫在 docs 的驗收清單裡,不假裝這裡做掉了。
 */
class KeyA11yTest {

    /* ─────────────── 1. 每一顆鍵都念得出名字 ─────────────── */

    /**
     * `core/layouts` 的**每一份**、每一層、每一列、每一顆非 spacer 的鍵。
     *
     * 空名字的下場是 TalkBack 只念「按鈕」,或者乾脆跳過 —— 使用者摸到一格
     * 沉默的方塊,不知道那是什麼,也不知道按下去會怎樣。
     */
    @Test
    fun `每一顆鍵都念得出一個名字`() {
        val repo = FixtureRepo()
        val silent = mutableListOf<String>()
        for (id in RepoFixtures.layoutIds) {
            val layout = repo.loadLayout(id).value ?: error("佈局 $id 載不起來")
            val labels = layout.layers.associate { it.id to it.label.get(LOCALE) }
            for (layer in layout.layers) {
                for (row in layer.rows) {
                    for (key in row.keys) {
                        if (key.spacer) continue
                        val name = KeyA11y.nameOf(key, labels)
                        val empty = name is KeyName.Face && name.text.isBlank()
                        if (empty) silent += "$id/${layer.id}/${key.id ?: "<無 id>"}"
                    }
                }
            }
        }
        assertTrue(
            "這幾顆鍵 TalkBack 念不出名字,只會念「按鈕」或直接跳過:\n  " +
                silent.joinToString("\n  "),
            silent.isEmpty(),
        )
    }

    /** 工具列項目同理 —— 它是使用者換鍵盤、開設定的唯一入口。 */
    @Test
    fun `工具列的每一項都念得出一個名字`() {
        val silent = mutableListOf<String>()
        for (id in themeIds()) {
            val theme = ThemeLoader.load(id, RepoFixtures.themes, Platform.ANDROID, locale = LOCALE)
                .value ?: error("主題 $id 載不起來")
            for (item in theme.candidates.bar.toolbar.items) {
                val name = KeyA11y.toolbarNameOf(item.icon, item.label, item.labelFrom, item.tap)
                if (name is KeyName.Face && name.text.isBlank()) {
                    silent += "$id 的 ${item.tap.raw}"
                }
            }
        }
        assertTrue("工具列這幾項念不出名字:\n  " + silent.joinToString("\n  "), silent.isEmpty())
    }

    /**
     * §9.6 的語義圖示表與朗讀名表必須一樣長。
     *
     * 圖示是**畫面上唯一的資訊**:一顆 `icon: undo` 的鍵上只有「↶」,沒有文字。
     * 漏一個圖示不會讓畫面壞掉,只是那顆鍵在 TalkBack 裡變成念「↶」——
     * 一個念不出來的字元。
     */
    @Test
    fun `每一個語義圖示都有朗讀名`() {
        val unknown = KeyA11y.ICON_NAMES.filterNot { LayoutParser.isKnownIcon(it) }
        assertEquals("朗讀名表裡有規範沒有的圖示名(拼錯了?)", emptyList<String>(), unknown)

        // 反方向:實際被用到的圖示都要念得出來。規範的 KNOWN_ICONS 是私有的,
        // 所以掃真正出貨的那幾份檔案 —— 那才是使用者摸得到的集合。
        val used = mutableSetOf<String>()
        val repo = FixtureRepo()
        for (id in RepoFixtures.layoutIds) {
            val layout = repo.loadLayout(id).value ?: continue
            for (layer in layout.layers) {
                for (row in layer.rows) row.keys.forEach { k -> k.icon?.let { used += it } }
            }
        }
        for (id in themeIds()) {
            val theme = ThemeLoader.load(id, RepoFixtures.themes, Platform.ANDROID, locale = LOCALE)
                .value ?: continue
            theme.candidates.bar.toolbar.items.forEach { item -> item.icon?.let { used += it } }
        }
        val missing = (used - KeyA11y.ICON_NAMES).sorted()
        assertEquals(
            "出貨的佈局／主題用了這幾個圖示,但 KeyA11y 沒有對應的朗讀名 —— " +
                "TalkBack 會去念那個替代字形",
            emptyList<String>(),
            missing,
        )
    }

    /* ─────────────── 2. 念得出來之後,按得動 ─────────────── */

    /**
     * 有行為的鍵,語意上就要有對應的動作。
     *
     * [KeyA11y.actionsOf] 是那條規則;下一條測試才驗 [KeyView] 真的照做了。
     */
    @Test
    fun `每一顆有行為的鍵都要求了對應的語意動作`() {
        val repo = FixtureRepo()
        val mute = mutableListOf<String>()
        for (id in RepoFixtures.layoutIds) {
            val layout = repo.loadLayout(id).value ?: error("佈局 $id 載不起來")
            for (layer in layout.layers) {
                for (row in layer.rows) {
                    for (key in row.keys) {
                        if (key.spacer) continue
                        val actions = KeyA11y.actionsOf(key)
                        val at = "$id/${layer.id}/${key.id ?: key.label}"
                        if (key.hasTapBehavior && A11yAction.CLICK !in actions) {
                            mute += "$at 點得下去,卻沒有語意上的點擊"
                        }
                        if ((key.longPress != null || key.popup != null) &&
                            A11yAction.LONG_CLICK !in actions
                        ) {
                            mute += "$at 有長按行為,卻沒有語意上的長按"
                        }
                    }
                }
            }
        }
        assertTrue(mute.joinToString("\n  ", prefix = "\n  "), mute.isEmpty())
    }

    /**
     * [KeyView] 必須把那些動作**真的接上**,而且接到同一段觸發邏輯。
     *
     * ── 為什麼是掃原始碼 ────────────────────────────────────────────────
     * 這是接線,不是純函式:要真的驗,得起一個 Compose UI 測試送 ACTION_CLICK。
     * 那值得做,但它擋不住這裡要擋的東西 —— 有人把 `onClick` 從語意區塊裡刪掉,
     * 畫面、手勢、既有測試全部不受影響,只有 TalkBack 使用者按不動。
     * 本專案已有同樣的先例(StringCatalogTest 直接讀 res、DeadKeyTest 掃分派表)。
     */
    @Test
    fun `KeyView 把語意動作接到同一段觸發邏輯`() {
        val src = File(
            RepoFixtures.coreDir.parentFile,
            "android/app/src/main/java/org/rimequad/ime/keyboard/KeyboardView.kt",
        )
        assertTrue("找不到 ${src.path} —— 檔案搬家就要改這裡,不能讓這條靜靜地不跑", src.isFile)
        val text = src.readText(Charsets.UTF_8)

        val semantics = text.substringAfter(".clearAndSetSemantics {", "")
            .substringBefore(".pointerInput(key)")
        assertTrue(
            "KeyView 裡找不到按鍵的語意區塊 —— 這條測試已經失效,先修它",
            semantics.isNotBlank() && semantics.length < 2000,
        )

        assertTrue(
            "語意區塊沒有 contentDescription,TalkBack 念不出這顆鍵",
            "contentDescription" in semantics,
        )
        assertTrue(
            "語意區塊沒有 onClick —— 鍵念得出來,輕點兩下卻什麼都不會發生",
            "onClick" in semantics,
        )
        assertTrue(
            "語意上的點擊沒有接到 fire(),等於另外寫了一條會漂移的路徑",
            Regex("""onClick\([^)]*\)\s*\{\s*fire\(\)""").containsMatchIn(semantics),
        )
        assertTrue(
            "語意區塊沒有 onLongClick —— 長按鍵與彈出盤對 TalkBack 是不存在的",
            "onLongClick" in semantics,
        )
        assertTrue(
            "沒有 Role.Button,TalkBack 不會提示「輕點兩下即可啟動」",
            "Role.Button" in semantics,
        )
    }

    /* ────────────────────────────── 夾具 ────────────────────────────── */

    private fun themeIds(): List<String> =
        File(RepoFixtures.coreDir, "themes").listFiles().orEmpty()
            .filter { it.isFile && it.name.endsWith(".yaml") }
            .map { it.name.removeSuffix(".yaml") }
            .sorted()

    private companion object {
        const val LOCALE = "zh-Hant-TW"
    }
}
