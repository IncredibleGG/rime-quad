package org.luminakey.ime.store

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.net.NetworkGate

class SchemaIndexTest {

    private fun ok(text: String): SchemaIndex {
        val r = IndexParser.parse(text)
        assertTrue("解析失敗: $r", r is IndexParseResult.Ok)
        return (r as IndexParseResult.Ok).index
    }

    /* ───────────────────── 假索引（也是真索引的對照組）───────────────────── */

    @Test
    fun `讀得進 make_fake_store 產生的索引`() {
        val idx = ok(StoreFixtures.indexText)
        assertEquals(1, idx.formatVersion)
        assertEquals(4, idx.packages.size)

        val demo = idx.packageById("rq-demo")!!
        assertEquals(listOf("rq-demo-base"), demo.requires)
        assertEquals(listOf("rq_demo"), demo.schemaIds)
        assertTrue(demo.verifiedDeployed)
        assertEquals("qwerty", demo.recommendedLayout)
        assertTrue(demo.recommended)
        assertEquals("nihao", demo.probe?.keys)
        assertEquals("你好", demo.probe?.expect)
        assertEquals(64, demo.sha256.length)

        // schemas 為空 = 只作為相依的元件套件（規範 §1）
        assertTrue(idx.packageById("rq-demo-base")!!.isComponentOnly)
    }

    @Test
    fun `hidden 類別不出現在市集列表`() {
        val idx = ok(StoreFixtures.indexText)
        val visible = idx.visibleCategories().map { it.id }
        assertFalse("essential 是 hidden，不該顯示", "essential" in visible)
        assertEquals(listOf("mandarin", "topolect", "other"), visible)
    }

    @Test
    fun `推薦款排在同類別最前面`() {
        val idx = ok(StoreFixtures.indexText)
        assertEquals("rq-demo", idx.packagesIn("mandarin").first().id)
    }

    /* ───────────────────── 版本與容錯 ───────────────────── */

    @Test
    fun `format_version 不相容時整份拒收`() {
        val r = IndexParser.parse("""{"format_version": 2, "packages": []}""")
        assertTrue(r is IndexParseResult.Err)
        assertTrue((r as IndexParseResult.Err).message.contains("format_version"))
    }

    @Test
    fun `沒有 sha256 的套件出局，其餘照常`() {
        val r = IndexParser.parse(
            """
            {"format_version":1,"packages":[
              {"id":"good","file":"a.zip","sha256":"${"a".repeat(64)}"},
              {"id":"nohash","file":"b.zip"},
              {"id":"badhash","file":"c.zip","sha256":"zz"}
            ]}
            """.trimIndent()
        )
        val okr = r as IndexParseResult.Ok
        assertEquals(listOf("good"), okr.index.packages.map { it.id })
        assertEquals(2, okr.warnings.size)
    }

    @Test
    fun `重複的 id 只留第一個`() {
        val h = "b".repeat(64)
        val r = IndexParser.parse(
            """{"format_version":1,"packages":[
                 {"id":"x","name":"first","file":"a.zip","sha256":"$h"},
                 {"id":"x","name":"second","file":"b.zip","sha256":"$h"}]}"""
        ) as IndexParseResult.Ok
        assertEquals(1, r.index.packages.size)
        assertEquals("first", r.index.packages[0].name)
    }

    /* ───────────────────── 相依解析（規範 §3）───────────────────── */

    @Test
    fun `遞迴展開 requires 且相依排在前面`() {
        val idx = ok(StoreFixtures.indexText)
        val r = DependencyResolver.resolve(idx, listOf("rq-demo"), emptySet())
        val plan = (r as DependencyResolver.Result.Ok).plan
        assertEquals(listOf("rq-demo-base", "rq-demo"), plan.toDownload.map { it.id })
        assertEquals(
            idx.packageById("rq-demo")!!.size + idx.packageById("rq-demo-base")!!.size,
            plan.totalBytes,
        )
    }

    @Test
    fun `已安裝的相依被扣掉`() {
        val idx = ok(StoreFixtures.indexText)
        val r = DependencyResolver.resolve(idx, listOf("rq-demo"), setOf("rq-demo-base"))
        val plan = (r as DependencyResolver.Result.Ok).plan
        assertEquals(listOf("rq-demo"), plan.toDownload.map { it.id })
        assertEquals(listOf("rq-demo-base"), plan.alreadyInstalled)
    }

    @Test
    fun `全部已安裝時計畫是空的`() {
        val idx = ok(StoreFixtures.indexText)
        val r = DependencyResolver.resolve(idx, listOf("rq-demo"), setOf("rq-demo-base", "rq-demo"))
        assertEquals(0, (r as DependencyResolver.Result.Ok).plan.count)
    }

    @Test
    fun `循環相依不是錯誤，兩邊一起裝`() {
        // 真索引裡 luna-pinyin 與 stroke 互相 requires（互為反查詞庫），
        // 上游本來就要一起裝。若把循環當錯誤擋掉，這兩個最常用的套件
        // 就永遠裝不了；照字面「遞迴展開」則會無限遞迴。
        val h = "c".repeat(64)
        val idx = ok(
            """{"format_version":1,"packages":[
                 {"id":"luna-pinyin","file":"a.zip","sha256":"$h","requires":["stroke"]},
                 {"id":"stroke","file":"b.zip","sha256":"$h","requires":["luna-pinyin"]}]}"""
        )
        val r = DependencyResolver.resolve(idx, listOf("luna-pinyin"), emptySet())
        assertTrue("循環必須被視為合法：$r", r is DependencyResolver.Result.Ok)
        val plan = (r as DependencyResolver.Result.Ok).plan
        assertEquals(
            setOf("luna-pinyin", "stroke"),
            plan.toDownload.map { it.id }.toSet(),
        )
        assertEquals("每個套件只能收一次", 2, plan.toDownload.size)
        assertTrue("循環要被記錄下來供 UI 說明", plan.cycles.isNotEmpty())
    }

    @Test
    fun `三個套件繞成一圈也不會無限遞迴`() {
        val h = "e".repeat(64)
        val idx = ok(
            """{"format_version":1,"packages":[
                 {"id":"a","file":"a.zip","sha256":"$h","requires":["b"]},
                 {"id":"b","file":"b.zip","sha256":"$h","requires":["c"]},
                 {"id":"c","file":"c.zip","sha256":"$h","requires":["a"]}]}"""
        )
        val plan = (DependencyResolver.resolve(idx, listOf("a"), emptySet())
            as DependencyResolver.Result.Ok).plan
        assertEquals(3, plan.toDownload.size)
    }

    @Test
    fun `安裝後佔用是下載量的概估倍數`() {
        val idx = ok(StoreFixtures.indexText)
        val plan = (DependencyResolver.resolve(idx, listOf("rq-demo"), emptySet())
            as DependencyResolver.Result.Ok).plan
        // 索引的 size 是 zip 大小；部署後的 build 產物遠大於它
        // （實測 luna-pinyin：0.4MB zip → 13MB）。UI 必須分開表達。
        assertTrue(plan.estimatedInstalledBytes > plan.totalBytes)
        assertEquals(
            plan.totalBytes * DependencyResolver.INSTALLED_SIZE_MULTIPLIER,
            plan.estimatedInstalledBytes,
        )
    }

    @Test
    fun `相依不在索引裡時指名是誰要的`() {
        val h = "d".repeat(64)
        val idx = ok(
            """{"format_version":1,"packages":[
                 {"id":"a","file":"a.zip","sha256":"$h","requires":["ghost"]}]}"""
        )
        val r = DependencyResolver.resolve(idx, listOf("a"), emptySet())
        assertTrue(r is DependencyResolver.Result.MissingDependency)
        assertEquals("ghost", (r as DependencyResolver.Result.MissingDependency).missing)
        assertEquals("a", r.requiredBy)
    }

    @Test
    fun `解除安裝前找得出誰還依賴它`() {
        val idx = ok(StoreFixtures.indexText)
        assertEquals(
            listOf("rq-demo"),
            DependencyResolver.dependentsOf(idx, "rq-demo-base", setOf("rq-demo", "rq-demo-base")),
        )
        assertEquals(
            emptyList<String>(),
            DependencyResolver.dependentsOf(idx, "rq-demo-base", setOf("rq-demo-base")),
        )
    }

    /* ───────────────────── URL 解析 ───────────────────── */

    @Test
    fun `套件 URL 以 base_url 為基底`() {
        assertEquals(
            "https://cdn.example/rime/x.zip",
            NetworkGate.resolveUrl(
                "https://cdn.example/rime/index.json", "https://cdn.example/rime/", "x.zip"
            ),
        )
    }

    @Test
    fun `沒有 base_url 時以索引檔的位置為基底`() {
        assertEquals(
            "https://cdn.example/rime/x.zip",
            NetworkGate.resolveUrl("https://cdn.example/rime/index.json", null, "x.zip"),
        )
    }

    @Test
    fun `file 本身是完整 URL 時直接用`() {
        assertEquals(
            "https://other.example/x.zip",
            NetworkGate.resolveUrl("https://cdn.example/index.json", "https://cdn.example/",
                "https://other.example/x.zip"),
        )
    }

    @Test
    fun `位元組格式化`() {
        assertEquals("962 KB", formatBytes(985_088))
        assertEquals("5.7 MB", formatBytes(5_976_883))
        assertEquals("12 B", formatBytes(12))
    }
}
