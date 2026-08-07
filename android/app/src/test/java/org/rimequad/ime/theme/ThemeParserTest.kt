package org.rimequad.ime.theme

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 對照 docs/theme-format.md §10「一致性檢核清單」。
 * 其他三端（Swift / C++ / C#）的解析器必須通過等價的斷言。
 */
class ThemeParserTest {
    private fun assertF(expected: Float, actual: Float) =
        assertEquals(expected.toDouble(), actual.toDouble(), 0.001)

    private fun assertF(message: String, expected: Float, actual: Float) =
        assertEquals(message, expected.toDouble(), actual.toDouble(), 0.001)


    private fun loadShipped(id: String): LoadResult<Theme> =
        ThemeLoader.load(id, RepoFixtures.themes, Platform.ANDROID)

    private fun loadInline(id: String, vararg docs: Pair<String, String>): LoadResult<Theme> =
        ThemeLoader.load(id, MapDocumentSource(docs.toMap()), Platform.ANDROID)

    // ── 檢核 1 ────────────────────────────────────────────────────────────

    @Test
    fun allShippedThemesParseWithoutDiagnostics() {
        for (id in RepoFixtures.themeIds) {
            val r = loadShipped(id)
            assertNotNull("$id failed to load: ${RepoFixtures.describe(r.diagnostics)}", r.value)
            assertEquals(
                "$id produced diagnostics: ${RepoFixtures.describe(r.diagnostics)}",
                0, r.diagnostics.size
            )
        }
    }

    @Test
    fun defaultThemesExposeTheExpectedShape() {
        val dark = loadShipped("default-dark").value!!
        assertEquals(Appearance.DARK, dark.appearance)
        assertEquals("default-light", dark.counterpart)
        assertEquals(0xFF101216.toInt(), dark.palette["bg"])
        assertEquals(0xFF4C8DFF.toInt(), dark.candidates.shared.item.highlightBackground)
        assertF(44f, dark.candidates.bar.height)
        assertEquals(Backdrop.NONE, dark.candidates.window.backdrop)
        assertF(0.33f, dark.keyboard.height.portrait.ratio)

        val light = loadShipped("default-light").value!!
        assertEquals(Appearance.LIGHT, light.appearance)
        assertEquals("default-dark", light.counterpart)
        // 深淺兩份刻意在「結構性」欄位上不同，證明 §8.2 的兩檔決策。
        assertF(1f, light.candidates.bar.borderTopWidth)
        assertF(0f, dark.candidates.bar.borderTopWidth)
        assertF(3f, light.metrics.elevation)
        assertF(2f, dark.metrics.elevation)
    }

    // ── 檢核 2：繼承 ───────────────────────────────────────────────────────

    @Test
    fun inheritanceMergesMappingsAndReplacesSequences() {
        val r = loadShipped("sakura-dark")
        val t = r.value!!
        assertEquals(listOf("default-dark", "sakura-dark"), t.ancestry)

        // palette 是映射：自身覆寫 accent/bg，其餘沿用父主題。
        assertEquals(0xFFF5789E.toInt(), t.palette["accent"])
        assertEquals(0xFF151016.toInt(), t.palette["bg"])
        assertEquals(0xFFE6E9EF.toInt(), t.palette["fg"])
        assertEquals(0xFF7A8496.toInt(), t.palette["muted"])

        // 深層映射遞迴合併：只寫了 corner_radius，padding_h 仍是父主題的值。
        assertF(14f, t.candidates.shared.item.cornerRadius)
        assertF(10f, t.candidates.shared.item.paddingH)
        assertF(14f, t.candidates.window.cornerRadius)

        // 序列整體取代：父主題的 family 完全不參與合併。
        val family = t.typography.font("candidate").family
        assertEquals(listOf("Iansui", "\$system"), family)

        // script_fallback 是映射，只有 hant 這條清單被換掉，hans 仍在。
        val fallback = t.typography.font("candidate").scriptFallback
        assertEquals("Iansui", fallback["hant"]!![0])
        assertTrue(fallback.containsKey("hans"))
    }

    @Test
    fun explicitNullDeletesTheInheritedValue() {
        // default-light 的 accent style 有 border_width: 1 / border_color: "$accent@35%"；
        // sakura-light 用顯式 null 刪掉它們，欄位必須回到規範預設值（0 / transparent）。
        val parent = loadShipped("default-light").value!!
        assertF(1f, parent.keyboard.keyStyle("accent").borderWidth)
        assertFalse(parent.keyboard.keyStyle("accent").borderColor == ColorSpec.TRANSPARENT)

        val child = loadShipped("sakura-light").value!!
        assertF(0f, child.keyboard.keyStyle("accent").borderWidth)
        assertEquals(ColorSpec.TRANSPARENT, child.keyboard.keyStyle("accent").borderColor)
    }

    @Test
    fun inheritanceCycleIsFatal() {
        val r = loadInline(
            "cyc-a",
            "cyc-a" to "format: rime-theme/1\nid: cyc-a\ninherits: cyc-b\n",
            "cyc-b" to "format: rime-theme/1\nid: cyc-b\ninherits: cyc-a\n"
        )
        assertNull(r.value)
        assertTrue(r.errors.any { it.message.contains("F6") })
    }

    @Test
    fun selfInheritanceIsFatalAndDoesNotRecurse() {
        val r = loadInline("selfie", "selfie" to "format: rime-theme/1\nid: selfie\ninherits: selfie\n")
        assertNull(r.value)
        assertTrue(r.errors.any { it.message.contains("F6") })
    }

    @Test
    fun missingParentIsFatal() {
        val r = loadInline("orphan", "orphan" to "format: rime-theme/1\nid: orphan\ninherits: nowhere\n")
        assertNull(r.value)
        assertTrue(r.errors.any { it.message.contains("F5") })
    }

    // ── 檢核 3–5：錯誤處理 ─────────────────────────────────────────────────

    @Test
    fun missingFormatIsFatal() {
        val r = loadInline("empty", "empty" to "{}\n")
        assertNull(r.value)
        assertTrue(r.errors.any { it.message.contains("F3") })
    }

    @Test
    fun wrongDocumentKindIsFatal() {
        val r = loadInline("wrong", "wrong" to "format: rime-layout/1\nid: wrong\n")
        assertNull(r.value)
        assertTrue(r.errors.any { it.message.contains("F3") })
    }

    @Test
    fun futureMajorVersionIsFatalWithAReadableMessage() {
        val r = loadInline("future", "future" to "format: rime-theme/9\nid: future\n")
        assertNull(r.value)
        assertTrue(r.errors.any { it.message.contains("update the app") })
    }

    @Test
    fun idMustMatchTheDocumentName() {
        val r = loadInline("named", "named" to "format: rime-theme/1\nid: other\n")
        assertNull(r.value)
        assertTrue(r.errors.any { it.message.contains("F4") })
    }

    @Test
    fun minClientTooNewIsFatal() {
        val r = ThemeLoader.load(
            "needy",
            MapDocumentSource(mapOf("needy" to "format: rime-theme/1\nid: needy\nmin_client: \"9.0.0\"\n")),
            Platform.ANDROID,
            "en",
            "0.1.0"
        )
        assertNull(r.value)
        assertTrue(r.errors.any { it.message.contains("F7") })
    }

    @Test
    fun invalidColorFallsBackToTheFieldDefaultWithExactlyOneWarning() {
        val r = loadInline(
            "badcolor",
            "badcolor" to """
                format: rime-theme/1
                id: badcolor
                appearance: light
                candidates:
                  item:
                    highlight_background: "#ZZZ"
            """.trimIndent()
        )
        val t = r.value
        assertNotNull("a bad color must not be fatal", t)
        assertEquals(RepoFixtures.describe(r.diagnostics), 1, r.diagnostics.size)
        assertEquals(Severity.WARNING, r.diagnostics[0].severity)
        assertEquals(
            "candidates.item.highlight_background",
            r.diagnostics[0].path
        )
        // 規範明令禁止用洋紅之類的「除錯色」，必須是該欄位的預設值。
        assertEquals(
            ThemeDefaults.candidateStyle(ThemeDefaults.METRICS).item.highlightBackground,
            t!!.candidates.shared.item.highlightBackground
        )
    }

    @Test
    fun unresolvedPaletteReferenceFallsBackToTheFieldDefault() {
        val r = loadInline(
            "badref",
            "badref" to """
                format: rime-theme/1
                id: badref
                candidates:
                  text:
                    color: "${'$'}nosuch"
            """.trimIndent()
        )
        assertNotNull(r.value)
        assertEquals(1, r.diagnostics.size)
        assertEquals(Severity.WARNING, r.diagnostics[0].severity)
    }

    @Test
    fun unknownFieldWarnsButKeepsTheThemeUsable() {
        val r = loadInline(
            "unknown",
            "unknown" to """
                format: rime-theme/1
                id: unknown
                keyboard:
                  blahblah: 1
            """.trimIndent()
        )
        assertNotNull(r.value)
        assertEquals(RepoFixtures.describe(r.diagnostics), 1, r.diagnostics.size)
        assertEquals("keyboard.blahblah", r.diagnostics[0].path)
        assertTrue(r.diagnostics[0].message.contains("unknown field"))
    }

    @Test
    fun typoInAFieldNameSuggestsTheClosestMatch() {
        val r = loadInline(
            "typo",
            "typo" to """
                format: rime-theme/1
                id: typo
                keyboard:
                  row_spacing: 4
                  key_spaceing: 4
            """.trimIndent()
        )
        assertNotNull(r.value)
        assertEquals(1, r.diagnostics.size)
        assertTrue(r.diagnostics[0].message.contains("key_spacing"))
    }

    @Test
    fun outOfRangeValuesAreClampedNotRejected() {
        val r = loadInline(
            "clamp",
            "clamp" to """
                format: rime-theme/1
                id: clamp
                keyboard:
                  height:
                    portrait:
                      ratio: 4.0
            """.trimIndent()
        )
        val t = r.value
        assertNotNull(t)
        assertF(0.6f, t!!.keyboard.height.portrait.ratio)
        // 夾制**必須**留下痕跡：使用者寫了 4.0 卻拿到 0.6，靜默處理是不可接受的。
        assertEquals(RepoFixtures.describe(r.diagnostics), 1, r.diagnostics.size)
        assertEquals(Severity.WARNING, r.diagnostics[0].severity)
        assertEquals("keyboard.height.portrait.ratio", r.diagnostics[0].path)
        assertTrue(r.diagnostics[0].message.contains("clamped"))
    }

    @Test
    fun clampingReportsBothBoundsAndIntegerFieldsToo() {
        val r = loadInline(
            "clamp2",
            "clamp2" to """
                format: rime-theme/1
                id: clamp2
                typography:
                  font_scale_min: 0.1
                keyboard:
                  popup:
                    max_columns: 99
                motion:
                  candidate_change_ms: 99999
            """.trimIndent()
        )
        val t = r.value!!
        assertF(0.5f, t.typography.fontScaleMin)   // 下界夾制
        assertEquals(12, t.keyboard.popup.maxColumns)   // 上界夾制（int）
        assertEquals(5000, t.motion.candidateChangeMs)  // duration 上界
        assertEquals(RepoFixtures.describe(r.diagnostics), 3, r.diagnostics.size)
        assertTrue(r.diagnostics.all { it.severity == Severity.WARNING })
        assertTrue(r.diagnostics.all { it.message.contains("clamped") })
    }

    @Test
    fun wrongTypesCoerceOrFallBack() {
        val r = loadInline(
            "types",
            "types" to """
                format: rime-theme/1
                id: types
                motion:
                  enabled: "yes"
                  key_press_ms: "50"
                  curve: "wobbly"
            """.trimIndent()
        )
        val t = r.value!!
        assertTrue(t.motion.enabled)          // "yes" 是合法布林
        assertEquals(50, t.motion.keyPressMs) // 字串數字可字面轉換
        assertEquals(MotionCurve.STANDARD, t.motion.curve) // 未知列舉值 → 預設 + WARNING
        assertEquals(1, r.diagnostics.size)
    }

    // ── 預設值與最小文件 ───────────────────────────────────────────────────

    @Test
    fun appendixAMinimalThemeLoadsEverywhere() {
        val r = loadInline(
            "minimal",
            "minimal" to """
                format: rime-theme/1
                id: minimal
                appearance: light
                palette:
                  accent: "#2563EB"
                candidates:
                  item:
                    highlight_background: "${'$'}accent"
            """.trimIndent()
        )
        val t = r.value
        assertNotNull(t)
        assertEquals(0, r.diagnostics.size)
        assertEquals(0xFF2563EB.toInt(), t!!.candidates.shared.item.highlightBackground)
        // 其餘全部走預設值
        assertF(ThemeDefaults.METRICS.cornerRadius, t.metrics.cornerRadius)
        assertF(44f, t.candidates.bar.height)
        assertTrue(t.motion.enabled)
        // §8.4 / §8.8.1 規定必須補齊的具名條目
        for (n in ThemeDefaults.REQUIRED_FONT_NAMES) assertTrue(n, t.typography.fonts.containsKey(n))
        for (n in ThemeDefaults.REQUIRED_KEY_STYLES) assertTrue(n, t.keyboard.keyStyles.containsKey(n))
    }

    // ── §8.6.6.1 工具列 ───────────────────────────────────────────────────

    @Test
    fun toolbarDefaultsToTheNormativeItemListWhenAbsent() {
        val r = loadInline("notoolbar", "notoolbar" to "format: rime-theme/1\nid: notoolbar\n")
        val tb = r.value!!.candidates.bar.toolbar
        assertEquals(0, r.diagnostics.size)
        assertTrue(tb.show)
        assertEquals(6, tb.items.size)
        assertEquals(ThemeDefaults.TOOLBAR_ITEMS, tb.items)
        assertTrue(tb.hasAction(ActionVerb.SCHEMA_PICKER))
        assertTrue(tb.hasAction(ActionVerb.SETTINGS))
    }

    @Test
    fun shippedThemesSpellOutTheSameToolbarAsTheDefaults() {
        // default-light / default-dark 是規範的對照組，寫出來的必須等於預設值。
        for (id in listOf("default-light", "default-dark")) {
            val tb = loadShipped(id).value!!.candidates.bar.toolbar
            assertEquals(id, ThemeDefaults.TOOLBAR_ITEMS, tb.items)
        }
    }

    @Test
    fun toolbarRestoresRequiredItemsAThemeTriedToRemove() {
        val r = loadInline(
            "slimbar",
            "slimbar" to """
                format: rime-theme/1
                id: slimbar
                candidates:
                  bar:
                    toolbar:
                      items:
                        - { icon: "emoji", tap: "emoji" }
            """.trimIndent()
        )
        val tb = r.value!!.candidates.bar.toolbar
        // 主題只留了 emoji，但方案切換與設定必須被補回來。
        assertEquals(3, tb.items.size)
        assertEquals(ActionVerb.EMOJI, tb.items[0].tap.verb)
        assertTrue(tb.hasAction(ActionVerb.SCHEMA_PICKER))
        assertTrue(tb.hasAction(ActionVerb.SETTINGS))
        // 補回來要留下痕跡，但那是 INFO 而非 WARNING —— 主題仍然完全可用。
        assertEquals(RepoFixtures.describe(r.diagnostics), 2, r.diagnostics.size)
        assertTrue(r.diagnostics.all { it.severity == Severity.INFO })
    }

    @Test
    fun toolbarItemWithoutATapActionIsDroppedNotFatal() {
        val r = loadInline(
            "badbar",
            "badbar" to """
                format: rime-theme/1
                id: badbar
                candidates:
                  bar:
                    toolbar:
                      items:
                        - { icon: "globe", tap: "schema:picker" }
                        - { icon: "settings", tap: "settings" }
                        - { icon: "emoji" }
                        - { label: "x", tap: "explode:now" }
            """.trimIndent()
        )
        val tb = r.value!!.candidates.bar.toolbar
        assertEquals(2, tb.items.size)
        assertEquals(2, r.diagnostics.size)
        assertTrue(r.diagnostics.all { it.severity == Severity.WARNING })
    }

    @Test
    fun toolbarItemsReuseTheLayoutActionAndLabelVocabulary() {
        // 工具列項目就是「沒有 send 的鍵」——不該有第二套詞彙。
        val defaults = ThemeDefaults.TOOLBAR_ITEMS
        val lang = defaults.first { it.labelFrom == LabelSource.INPUT_MODE }
        assertEquals(ActionVerb.TOGGLE_OPTION, lang.tap.verb)
        assertEquals("ascii_mode", lang.tap.arg)
        assertNull(lang.icon)
        val picker = defaults.first { it.tap.verb == ActionVerb.SCHEMA_PICKER }
        assertEquals("globe", picker.icon)
        assertTrue(LayoutParser.isKnownIcon(picker.icon!!))
    }

    @Test
    fun keysymFallbackSeamDefersUnknownNamesToNative() {
        // §9.4：靜態表查不到時必須回落，而不是把該鍵當成 noop。
        assertEquals(Keysym.VOID_SYMBOL, Keysym.resolve("Cyrillic_a"))
        assertEquals(0x6C1, Keysym.resolveWith("Cyrillic_a") { 0x6C1 })
        // 靜態表命中時不得呼叫 native。
        assertEquals(0x31, Keysym.resolveWith("1") { throw AssertionError("native must not be consulted") })
        // 尚未接上 native 時退化為 VOID（可觀察的過渡期偏離）。
        assertEquals(Keysym.VOID_SYMBOL, Keysym.resolveWith("Cyrillic_a", null))
    }

    @Test
    fun bothNotFoundSentinelsAreNormalisedAtTheBoundary() {
        // rs_keysym_by_name() 查不到時回傳 0；靜態表用 0xFFFFFF。
        // 兩者都必須被擋下來，絕不能當成有效 keysym 送進 rs_process_key()。
        assertEquals(Keysym.VOID_SYMBOL, Keysym.resolveWith("NoSuchKey") { 0 })
        assertEquals(Keysym.VOID_SYMBOL, Keysym.resolveWith("NoSuchKey") { Keysym.VOID_SYMBOL })
        // 有效值原樣通過。
        assertEquals(0x6C1, Keysym.resolveWith("NoSuchKey") { 0x6C1 })
    }

    @Test
    fun alphaModulationMultipliesRatherThanOverwrites() {
        assertEquals(
            0x80FF0000.toInt(),
            ColorSpec.resolve("\$c@50%", mapOf("c" to 0xFFFF0000.toInt()))
        )
        // 基色本身已半透明時，@50% 再乘一次。
        assertEquals(
            0x40FF0000,
            ColorSpec.resolve("\$c@50%", mapOf("c" to 0x80FF0000.toInt()))
        )
        assertEquals(0xFFFF00AA.toInt(), ColorSpec.parseHex("#f0a"))
        assertEquals(ColorSpec.TRANSPARENT, ColorSpec.resolve("transparent", emptyMap()))
        assertNull(ColorSpec.resolve("#12345", emptyMap()))
    }

    @Test
    fun platformOverridesApplyOnlyToTheCurrentPlatform() {
        val doc = """
            format: rime-theme/1
            id: plat
            keyboard:
              row_spacing: 6
            platform_overrides:
              ios:
                keyboard:
                  row_spacing: 2
              plan9:
                keyboard:
                  row_spacing: 99
        """.trimIndent()
        val src = MapDocumentSource(mapOf("plat" to doc))

        val android = ThemeLoader.load("plat", src, Platform.ANDROID).value!!
        assertF(6f, android.keyboard.rowSpacing)

        val ios = ThemeLoader.load("plat", src, Platform.IOS).value!!
        assertF(2f, ios.keyboard.rowSpacing)

        // 未知平台鍵必須被靜默忽略（前向相容）。
        assertEquals(0, ThemeLoader.load("plat", src, Platform.ANDROID).diagnostics.size)
    }

    @Test
    fun heightResolutionFollowsRatioThenClamp() {
        val h = HeightSpec(ratio = 0.33f, min = 190f, max = 300f)
        assertF(264f, h.resolve(800f))   // 0.33 * 800
        assertF(300f, h.resolve(1400f))  // 夾到 max
        assertF(190f, h.resolve(400f))   // 夾到 min
        assertF(330f, h.resolve(800f, 1.25f)) // height_scale 在夾制之後
    }

    @Test
    fun localizedStringFallsBackByLanguageSubtag() {
        val t = loadShipped("default-dark").value!!
        assertEquals("預設深色", t.name.get("zh-Hant-TW"))
        assertEquals("Default Dark", t.name.get("fr-FR"))
    }
}
