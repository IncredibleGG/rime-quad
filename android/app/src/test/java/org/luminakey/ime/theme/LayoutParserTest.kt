package org.luminakey.ime.theme

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.theme.DiagnosticCode

class LayoutParserTest {
    private fun assertF(expected: Float, actual: Float) =
        assertEquals(expected.toDouble(), actual.toDouble(), 0.001)

    private fun assertF(message: String, expected: Float, actual: Float) =
        assertEquals(message, expected.toDouble(), actual.toDouble(), 0.001)


    private fun loadShipped(id: String): LoadResult<KeyboardLayout> =
        LayoutLoader.load(id, RepoFixtures.layouts, Platform.ANDROID)

    private fun loadInline(id: String, vararg docs: Pair<String, String>): LoadResult<KeyboardLayout> =
        LayoutLoader.load(id, MapDocumentSource(docs.toMap()), Platform.ANDROID)

    private fun key(layout: KeyboardLayout, layerId: String, keyId: String): LayoutKey {
        val layer = layout.layer(layerId)!!
        for (row in layer.rows) for (k in row.keys) if (k.id == keyId) return k
        throw AssertionError("no key '$keyId' in layer '$layerId' of ${layout.id}")
    }

    // ── 檢核 1 ────────────────────────────────────────────────────────────

    @Test
    fun allShippedLayoutsParseWithoutDiagnostics() {
        for (id in RepoFixtures.layoutIds) {
            val r = loadShipped(id)
            assertNotNull("$id failed to load: ${RepoFixtures.describe(r.diagnostics)}", r.value)
            assertEquals(
                "$id produced diagnostics: ${RepoFixtures.describe(r.diagnostics)}",
                0, r.diagnostics.size
            )
        }
    }

    // ── 檢核 6：注音的 label / hint / send 三者分離 ────────────────────────

    @Test
    fun bopomofoSeparatesLabelFromKeysym() {
        val layout = loadShipped("bopomofo-dachen").value!!
        assertEquals(LayoutKind.PHONETIC, layout.kind)
        assertTrue(layout.matchesSchema("bopomofo"))
        assertTrue(!layout.matchesSchema("luna_pinyin"))

        // ㄅ 印在鍵面上，送出的卻是 keysym "1" (0x31)。
        val b = key(layout, "bopomofo", "b_")
        assertEquals("ㄅ", b.label)   // ㄅ
        assertEquals("1", b.hint)
        val send = b.send as SendSpec.Keysym
        assertEquals("1", send.name)
        assertEquals(0x31, send.code)
        assertEquals(Modifiers.NONE, send.modifiers)

        // ㄆ 送 'q'，ㄇ 送 'a'，ㄈ 送 'z' —— 大千排列整條對角線。
        assertEquals(0x71, (key(layout, "bopomofo", "p_").send as SendSpec.Keysym).code)
        assertEquals(0x61, (key(layout, "bopomofo", "m_").send as SendSpec.Keysym).code)
        assertEquals(0x7A, (key(layout, "bopomofo", "f_").send as SendSpec.Keysym).code)
        // ㄦ 在 '-' 上
        assertEquals(0x2D, (key(layout, "bopomofo", "er_").send as SendSpec.Keysym).code)
        // ㄥ 在 '/' 上
        assertEquals(0x2F, (key(layout, "bopomofo", "eng_").send as SendSpec.Keysym).code)
    }

    @Test
    fun bopomofoUsesFeaturesQwertyDoesNot() {
        val layout = loadShipped("bopomofo-dachen").value!!
        val main = layout.layer("bopomofo")!!

        // 11 欄網格（QWERTY 是 10）
        assertF(11.0f, main.units)
        // 逐列不同的 weight
        assertF(0.88f, main.rows[0].weight)
        assertF(1.0f, main.rows[1].weight)
        // 逐鍵的分數寬度權重
        assertF(1.1f, key(layout, "bopomofo", "f_").width)

        // 聲調鍵用 grid 型 popup
        val tone = key(layout, "bopomofo", "tone3")
        val popup = tone.popup!!
        assertEquals(PopupLayout.GRID, popup.layout)
        assertEquals(2, popup.columns)
        assertEquals(4, popup.keys.size)
        assertEquals(0x36, (popup.keys[0].send as SendSpec.Keysym).code) // ˊ → '6'
        // 同一顆鍵上的 swipe
        assertEquals(0x34, (tone.swipe[SwipeDirection.UP]!!.send as SendSpec.Keysym).code)

        // 同檔第二層 + send.text 全形標點
        val punct = layout.layer("punct")!!
        assertF(11.0f, punct.units)
        assertEquals("，", (key(layout, "punct", "p_comma").send as SendSpec.Text).text)

        // 空白鍵同時承擔一聲與選字確認 —— 前端不分支，老實送 keysym space。
        val space = key(layout, "bopomofo", "space")
        assertEquals(0x20, (space.send as SendSpec.Keysym).code)
        assertEquals(LabelSource.SCHEMA_NAME, space.labelFrom)
        assertEquals(ActionVerb.CANDIDATE_NEXT_PAGE, space.swipe[SwipeDirection.UP]!!.tap!!.verb)
    }

    // ── 九宮格：證明格式不需要「一鍵多值」 ─────────────────────────────────

    @Test
    fun ninekeyNeedsNoMultiValueKeyExtension() {
        val layout = loadShipped("t9-pinyin").value!!
        assertEquals(LayoutKind.GRID, layout.kind)
        assertEquals(4.0f, layout.layer("t9")!!.units, 0.001f)

        // 「ABC」鍵送出的是**單一** keysym 'A'(0x41)，不是三個字元。
        // 模糊比對由 T9 方案的 prism 承擔，前端只送一個鍵 ——
        // 這證明佈局格式不需要「一鍵多值」的擴充。
        val abc = key(layout, "t9", "k2")
        assertEquals("ABC", abc.label)
        assertEquals("2", abc.hint)          // hint 仍是使用者熟悉的數字
        val send = abc.send as SendSpec.Keysym
        assertEquals(0x41, send.code)
        assertEquals(Modifiers.NONE, send.modifiers)

        // 八顆字母鍵各送一個代表大寫字母（YuyanIme 驗證過的 speller alphabet）。
        for ((id, code) in listOf(
            "k2" to 0x41, "k3" to 0x44, "k4" to 0x47, "k5" to 0x4A,
            "k6" to 0x4D, "k7" to 0x50, "k8" to 0x54, "k9" to 0x57
        )) {
            assertEquals(id, code, (key(layout, "t9", id).send as SendSpec.Keysym).code)
        }

        // 「你好」的按鍵序列 M G G A M —— 方案落地後照這個序列實測。
        val nihao = listOf("k6", "k4", "k4", "k2", "k6")
        val codes = nihao.map { (key(layout, "t9", it).send as SendSpec.Keysym).code }
        assertEquals(listOf(0x4D, 0x47, 0x47, 0x41, 0x4D), codes)
    }

    // ── 檢核 7：列寬總和 ──────────────────────────────────────────────────

    @Test
    fun everyRowFillsItsLayerUnits() {
        for (id in RepoFixtures.layoutIds) {
            val layout = loadShipped(id).value!!
            for (layer in layout.layers) {
                for ((i, row) in layer.rows.withIndex()) {
                    assertEquals(
                        "$id/${layer.id} row $i",
                        layer.units.toDouble(), row.widthSum.toDouble(), 0.01
                    )
                }
            }
        }
    }

    // ── QWERTY ───────────────────────────────────────────────────────────

    @Test
    fun qwertyShiftLayerSendsUppercaseKeysymsWithShift() {
        val layout = loadShipped("qwerty").value!!
        assertTrue(layout.primary)
        assertEquals("lower", layout.defaultLayer)

        val lowerQ = key(layout, "lower", "q").send as SendSpec.Keysym
        assertEquals(0x71, lowerQ.code)
        assertEquals(Modifiers.NONE, lowerQ.modifiers)

        val upperQ = key(layout, "upper", "q").send as SendSpec.Keysym
        assertEquals(0x51, upperQ.code)
        assertEquals(Modifiers.SHIFT, upperQ.modifiers)

        val shift = key(layout, "lower", "shift")
        assertEquals(ActionVerb.LAYER_ONCE, shift.tap!!.verb)
        assertEquals("upper", shift.tap!!.arg)
        assertEquals(ActionVerb.LAYER_LOCK, shift.doubleTap!!.verb)
        assertTrue(key(layout, "upper", "shift").active)
    }

    @Test
    fun backspaceGoesThroughTheEngineAndRepeats() {
        val layout = loadShipped("qwerty").value!!
        val bs = key(layout, "lower", "backspace")
        assertEquals("BackSpace", (bs.send as SendSpec.Keysym).name)
        assertEquals(0xFF08, (bs.send as SendSpec.Keysym).code)
        assertTrue(bs.repeat)
        assertNull("repeat and long_press are mutually exclusive", bs.longPress)
        assertEquals(ActionVerb.CLEAR, bs.swipe[SwipeDirection.LEFT]!!.tap!!.verb)
    }

    @Test
    fun spacersAreInertButOccupyWidth() {
        val layout = loadShipped("qwerty").value!!
        val row = layout.layer("lower")!!.rows[1]
        assertTrue(row.keys[0].spacer)
        assertF(0.5f, row.keys[0].width)
        assertNull(row.keys[0].send)
        assertNull(row.keys[0].tap)
    }

    // ── numeric-symbol：keysym 與 text 的分工 ─────────────────────────────

    @Test
    fun asciiPunctuationGoesThroughLibrimeAndExoticCharactersDoNot() {
        val layout = loadShipped("numeric-symbol").value!!
        // ',' 走 keysym，讓 librime 的 punctuator 依 ascii_punct 決定要不要轉全形。
        val comma = key(layout, "numeric", "comma").send as SendSpec.Keysym
        assertEquals("comma", comma.name)
        assertEquals(0x2C, comma.code)
        // '€' librime 沒有對應 keysym，直接上屏。
        assertEquals("€", (key(layout, "symbol", "euro").send as SendSpec.Text).text)
        // 跨檔切換回**來的地方**。這裡曾經是 @primary,而那是一個缺陷:
        // 符號佈局是從九宮格／注音「進來」的附屬佈局,一律送回 qwerty 等於
        // 把那些使用者鎖在 qwerty 上(qwerty 沒有任何一顆鍵能回九宮格)。
        val abc = key(layout, "numeric", "to_alpha")
        assertEquals(ActionVerb.SWITCH_LAYOUT, abc.tap!!.verb)
        assertEquals("@previous", abc.tap!!.arg)
    }

    // ── 繼承與 key_patches ────────────────────────────────────────────────

    private val baseLayout = """
        format: rime-layout/1
        id: base-layout
        primary: true
        layers:
          - id: main
            units: 3
            rows:
              - keys:
                  - { id: "a", label: "a", send: { keysym: "a" } }
                  - { id: "b", label: "b", send: { keysym: "b" } }
                  - { id: "z", label: "z", send: { keysym: "z" }, long_press: "settings" }
    """.trimIndent()

    @Test
    fun keyPatchesEditIndividualKeysOfAnInheritedLayout() {
        val child = """
            format: rime-layout/1
            id: child-layout
            inherits: base-layout
            key_patches:
              b:
                label: "B"
                send: { keysym: "B", modifiers: [Shift] }
              z:
                long_press: null
        """.trimIndent()
        val r = loadInline("child-layout", "base-layout" to baseLayout, "child-layout" to child)
        val layout = r.value
        assertNotNull(RepoFixtures.describe(r.diagnostics), layout)
        assertEquals(RepoFixtures.describe(r.diagnostics), 0, r.diagnostics.size)

        assertEquals("B", key(layout!!, "main", "b").label)
        val sent = key(layout, "main", "b").send as SendSpec.Keysym
        assertEquals(0x42, sent.code)
        assertEquals(Modifiers.SHIFT, sent.modifiers)
        // 未被 patch 的鍵原封不動
        assertEquals("a", key(layout, "main", "a").label)
        // 顯式 null 刪掉了父佈局的 long_press
        assertNull(key(layout, "main", "z").longPress)
        // 非繼承欄位取自葉文件
        assertEquals(listOf("base-layout", "child-layout"), layout.ancestry)
    }

    @Test
    fun keyPatchForAMissingIdWarnsButIsNotFatal() {
        val child = """
            format: rime-layout/1
            id: child2
            inherits: base-layout
            key_patches:
              nosuchkey:
                label: "x"
        """.trimIndent()
        val r = loadInline("child2", "base-layout" to baseLayout, "child2" to child)
        assertNotNull(r.value)
        assertEquals(1, r.diagnostics.size)
        assertEquals("key_patches.nosuchkey", r.diagnostics[0].path)
    }

    // ── 錯誤處理 ─────────────────────────────────────────────────────────

    @Test
    fun layoutWithoutLayersIsFatal() {
        val r = loadInline(
            "nolayers-missing",
            "nolayers-missing" to "format: rime-layout/1\nid: nolayers-missing\n"
        )
        assertNull(r.value)
        assertEquals(listOf(DiagnosticCode.FATAL_LAYERS_MISSING), r.errors.map { it.code })
    }

    @Test
    fun emptyLayersIsFatalWithF8() {
        val r = loadInline("nolayers", "nolayers" to "format: rime-layout/1\nid: nolayers\nlayers: []\n")
        assertNull(r.value)
        assertEquals(listOf(DiagnosticCode.FATAL_LAYERS_MISSING), r.errors.map { it.code })
    }

    @Test
    fun unknownDefaultLayerIsFatalWithF9() {
        val doc = """
            format: rime-layout/1
            id: badlayer
            default_layer: nowhere
            layers:
              - id: main
                rows:
                  - keys:
                      - { label: "a", send: { keysym: "a" } }
        """.trimIndent()
        val r = loadInline("badlayer", "badlayer" to doc)
        assertNull(r.value)
        assertEquals(listOf(DiagnosticCode.FATAL_DEFAULT_LAYER_UNKNOWN), r.errors.map { it.code })
    }

    @Test
    fun unknownActionDegradesTheKeyToNoop() {
        val doc = """
            format: rime-layout/1
            id: badaction
            layers:
              - id: main
                rows:
                  - keys:
                      - { id: "k", label: "k", tap: "explode:now" }
                      - { id: "j", label: "j", tap: "layer:ghost" }
        """.trimIndent()
        val r = loadInline("badaction", "badaction" to doc)
        val layout = r.value
        assertNotNull(layout)
        assertNull(key(layout!!, "main", "k").tap)
        assertNull(key(layout, "main", "j").tap)
        assertEquals(2, r.diagnostics.size)
        assertTrue(r.diagnostics.all { it.severity == Severity.WARNING })
    }

    @Test
    fun sendAndTapAreMutuallyExclusive() {
        val doc = """
            format: rime-layout/1
            id: bothsend
            layers:
              - id: main
                rows:
                  - keys:
                      - { id: "k", label: "k", send: { keysym: "a" }, tap: "clear" }
        """.trimIndent()
        val r = loadInline("bothsend", "bothsend" to doc)
        val k = key(r.value!!, "main", "k")
        assertNull(k.send)
        assertEquals(ActionVerb.CLEAR, k.tap!!.verb)
        assertEquals(1, r.diagnostics.size)
    }

    @Test
    fun unitsDefaultToTheWidestRow() {
        val doc = """
            format: rime-layout/1
            id: autounits
            layers:
              - id: main
                rows:
                  - keys:
                      - { label: "a", send: { keysym: "a" } }
                      - { label: "b", send: { keysym: "b" } }
                      - { label: "c", send: { keysym: "c" } }
        """.trimIndent()
        val r = loadInline("autounits", "autounits" to doc)
        assertF(3.0f, r.value!!.layer("main")!!.units)
        assertEquals(0, r.diagnostics.size)
    }

    @Test
    fun keysymResolutionFollowsTheSpec() {
        assertEquals(0x31, Keysym.resolve("1"))          // 名稱，不是數值
        assertEquals(0x20, Keysym.resolve("space"))
        assertEquals(0xFF0D, Keysym.resolve("Return"))
        assertEquals(0xFF08, Keysym.resolve("BackSpace"))
        assertEquals(0x7E, Keysym.resolve("asciitilde"))
        assertEquals(0x01003105, Keysym.resolve("U+3105")) // ㄅ 的 Unicode keysym
        assertEquals(0xE9, Keysym.resolve("U+00E9"))       // Latin-1 直接用碼位
        assertEquals(0xFF08, Keysym.resolve("0xFF08"))
        assertEquals(Keysym.VOID_SYMBOL, Keysym.resolve("NoSuchKey"))
    }
}
