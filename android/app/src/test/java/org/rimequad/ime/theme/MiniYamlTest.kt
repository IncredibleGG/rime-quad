package org.rimequad.ime.theme

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

class MiniYamlTest {

    private fun map(text: String): YamlNode.Mapping =
        MiniYaml.parse("test.yaml", text).root as YamlNode.Mapping

    private fun scalar(n: YamlNode?): String? = (n as? YamlNode.Scalar)?.value

    @Test
    fun parsesBlockMappingsAndNesting() {
        val m = map(
            """
            a: 1
            b:
              c: hello
              d:
                e: deep
            """.trimIndent()
        )
        assertEquals("1", scalar(m.entries["a"]))
        val b = m.entries["b"] as YamlNode.Mapping
        assertEquals("hello", scalar(b.entries["c"]))
        val d = b.entries["d"] as YamlNode.Mapping
        assertEquals("deep", scalar(d.entries["e"]))
    }

    @Test
    fun parsesBlockSequencesOfCompactMappings() {
        val m = map(
            """
            rows:
              - weight: 1.0
                keys:
                  - label: "a"
                  - label: "b"
              - weight: 0.5
                keys:
                  - label: "c"
            """.trimIndent()
        )
        val rows = m.entries["rows"] as YamlNode.Sequence
        assertEquals(2, rows.items.size)
        val row0 = rows.items[0] as YamlNode.Mapping
        assertEquals("1.0", scalar(row0.entries["weight"]))
        val keys0 = row0.entries["keys"] as YamlNode.Sequence
        assertEquals(2, keys0.items.size)
        assertEquals("b", scalar((keys0.items[1] as YamlNode.Mapping).entries["label"]))
        val row1 = rows.items[1] as YamlNode.Mapping
        assertEquals(1, (row1.entries["keys"] as YamlNode.Sequence).items.size)
    }

    @Test
    fun parsesFlowCollections() {
        val m = map(
            """
            targets: [android, ios]
            send: { keysym: "1", modifiers: [Shift] }
            empty_map: {}
            empty_list: []
            """.trimIndent()
        )
        val targets = m.entries["targets"] as YamlNode.Sequence
        assertEquals(listOf("android", "ios"), targets.items.map { scalar(it) })
        val send = m.entries["send"] as YamlNode.Mapping
        assertEquals("1", scalar(send.entries["keysym"]))
        assertEquals(1, (send.entries["modifiers"] as YamlNode.Sequence).items.size)
        assertEquals(0, (m.entries["empty_map"] as YamlNode.Mapping).entries.size)
        assertEquals(0, (m.entries["empty_list"] as YamlNode.Sequence).items.size)
    }

    /** core/layouts/numeric-symbol.yaml 真的有跨行的 flow mapping。 */
    @Test
    fun parsesMultiLineFlowCollections() {
        val m = map(
            """
            keys:
              - { id: "dollar", label: "d", send: { keysym: "dollar" },
                  popup: { layout: row, keys: [ { label: "€", send: { text: "€" } } ] } }
            """.trimIndent()
        )
        val keys = m.entries["keys"] as YamlNode.Sequence
        assertEquals(1, keys.items.size)
        val k = keys.items[0] as YamlNode.Mapping
        assertEquals("dollar", scalar(k.entries["id"]))
        val popup = k.entries["popup"] as YamlNode.Mapping
        val sub = (popup.entries["keys"] as YamlNode.Sequence).items[0] as YamlNode.Mapping
        assertEquals("€", scalar((sub.entries["send"] as YamlNode.Mapping).entries["text"]))
    }

    @Test
    fun keepsHashInsideQuotesAndStripsComments() {
        val m = map(
            """
            # leading comment
            color: "#101216"   # trailing comment
            label: "#+="
            plain: value # cut here
            """.trimIndent()
        )
        assertEquals("#101216", scalar(m.entries["color"]))
        assertEquals("#+=", scalar(m.entries["label"]))
        assertEquals("value", scalar(m.entries["plain"]))
    }

    @Test
    fun handlesEscapesAndQuotedSpecials() {
        val m = map(
            """
            dq: "a\"b"
            bs: "\\"
            uni: "\u3105"
            sq: 'it''s'
            colonish: "a: b"
            """.trimIndent()
        )
        assertEquals("a\"b", scalar(m.entries["dq"]))
        assertEquals("\\", scalar(m.entries["bs"]))
        assertEquals("\u3105", scalar(m.entries["uni"]))
        assertEquals("it's", scalar(m.entries["sq"]))
        assertEquals("a: b", scalar(m.entries["colonish"]))
    }

    /**
     * 規範 §3.3 的核心保證：讀取層**不做隱式型別解析**。
     * `y` / `n` / `on` / `off` 在 YAML 1.1 是布林、在 1.2 是字串 —— 這裡一律是字串。
     */
    @Test
    fun doesNotResolveImplicitTypes() {
        val m = map(
            """
            a: y
            b: n
            c: on
            d: off
            e: true
            f: 0x1F
            """.trimIndent()
        )
        assertEquals("y", scalar(m.entries["a"]))
        assertEquals("n", scalar(m.entries["b"]))
        assertEquals("on", scalar(m.entries["c"]))
        assertEquals("off", scalar(m.entries["d"]))
        assertEquals("true", scalar(m.entries["e"]))
        assertEquals("0x1F", scalar(m.entries["f"]))
    }

    @Test
    fun nullFormsProduceNullScalars() {
        val m = map(
            """
            a: null
            b: ~
            c:
            d: NULL
            """.trimIndent()
        )
        assertNull(scalar(m.entries["a"]))
        assertNull(scalar(m.entries["b"]))
        assertNull(scalar(m.entries["c"]))
        assertNull(scalar(m.entries["d"]))
        assertTrue(m.entries.containsKey("a"))
    }

    @Test
    fun duplicateKeyWarnsAndLastWins() {
        val doc = MiniYaml.parse("test.yaml", "a: 1\na: 2\n")
        val m = doc.root as YamlNode.Mapping
        assertEquals("2", scalar(m.entries["a"]))
        assertEquals(1, doc.warnings.size)
        assertEquals(DiagnosticCode.DUPLICATE_KEY, doc.warnings[0].code)
        assertEquals(listOf("a"), doc.warnings[0].args)
    }

    @Test
    fun tabIndentationIsRejected() {
        try {
            map("a:\n\tb: 1\n")
            fail("expected a syntax error for tab indentation")
        } catch (e: YamlSyntaxException) {
            assertTrue(e.detail.contains("tab"))
        }
    }

    @Test
    fun anchorsAreRejected() {
        try {
            map("a: 1\n&anchor\n")
            fail("expected a syntax error for anchors")
        } catch (e: YamlSyntaxException) {
            assertTrue(e.detail.contains("anchor"))
        }
    }

    @Test
    fun everyShippedDocumentParses() {
        for (id in RepoFixtures.themeIds) {
            val text = RepoFixtures.themes.read(id)
            assertNotNull("missing theme $id", text)
            MiniYaml.parse("$id.yaml", text!!)
        }
        for (id in RepoFixtures.layoutIds) {
            val text = RepoFixtures.layouts.read(id)
            assertNotNull("missing layout $id", text)
            MiniYaml.parse("$id.yaml", text!!)
        }
    }
}
