package org.luminakey.ime.store

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.luminakey.ime.R
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * 帳本、以及「已安裝 ≠ 已啟用」這條分界。
 *
 * 分界的成本理由：librime 每次部署會編譯 schema_list 上的**所有**方案，
 * 實測三個方案的耗時見 core/DeployEstimate.kt，而且是累加的。
 * 停用必須只動 schema_list、**留著檔案**，
 * 否則使用者想再用就得重新下載一次。
 */
class InstalledRegistryTest {

    @get:Rule
    val tmp = TemporaryFolder()

    private fun entry(id: String, schema: String?, requires: List<String> = emptyList()) =
        InstalledPackage(
            id = id,
            name = "名稱 $id",
            sha256 = "a".repeat(64),
            installedAt = 1_700_000_000_000,
            schemas = schema?.let { listOf(StoreSchemaRef(it, "顯示 $it")) } ?: emptyList(),
            files = listOf("$id.schema.yaml", "LICENSE"),
            requires = requires,
            recommendedLayout = if (schema == "bopomofo_tw") "bopomofo-dachen" else "qwerty",
            layoutNote = null,
            source = "store",
        )

    @Test
    fun `寫出去再讀回來內容一致`() {
        val d = tmp.newFolder()
        val reg = InstalledRegistry.load(d)
        reg.put(entry("rq-demo-base", null))
        reg.put(entry("rq-demo", "rq_demo", requires = listOf("rq-demo-base")))

        val again = InstalledRegistry.load(d)
        assertEquals(setOf("rq-demo-base", "rq-demo"), again.ids)
        assertEquals(listOf("rq_demo"), again.get("rq-demo")!!.schemaIds)
        assertEquals(listOf("rq-demo-base"), again.get("rq-demo")!!.requires)
        assertEquals("qwerty", again.layoutForSchema("rq_demo"))
        assertNull(again.layoutForSchema("不存在"))
    }

    @Test
    fun `注音方案的 recommended_layout 查得到 —— 少了它使用者會看到 QWERTY 打不出注音`() {
        val d = tmp.newFolder()
        val reg = InstalledRegistry.load(d)
        reg.put(entry("rime-bopomofo", "bopomofo_tw"))
        assertEquals(
            "bopomofo-dachen",
            InstalledRegistry.load(d).layoutForSchema("bopomofo_tw"),
        )
    }

    @Test
    fun `找得出誰依賴誰`() {
        val d = tmp.newFolder()
        val reg = InstalledRegistry.load(d)
        reg.put(entry("base", null))
        reg.put(entry("leaf", "leaf_schema", requires = listOf("base")))
        assertEquals(listOf("leaf"), reg.dependents("base").map { it.id })
        assertEquals(emptyList<String>(), reg.dependents("leaf").map { it.id })
    }

    @Test
    fun `帳本壞掉時當作空的，不讓整個市集崩掉`() {
        val d = tmp.newFolder()
        File(d, InstalledRegistry.FILE_NAME).writeText("{ 這不是 JSON")
        assertEquals(emptySet<String>(), InstalledRegistry.load(d).ids)
    }

    /* ─────────────── SchemaStore 的本機匯入（不碰 librime）─────────────── */

    private fun store(): Triple<SchemaStore, File, File> {
        val user = tmp.newFolder("user")
        val shared = tmp.newFolder("shared")
        val work = tmp.newFolder("work")
        return Triple(SchemaStore(user, shared, work), user, shared)
    }

    @Test
    fun `匯入合法 zip 之後檔案落地且記進帳本`() {
        val (s, user, _) = store()
        val r = s.importLocal(StoreFixtures.packageZip("rq-demo"), "rq-demo.zip") {}
        assertTrue(r.toString(), r is SchemaStore.Outcome.Ok)
        assertTrue(File(user, "rq_demo.schema.yaml").isFile)
        val rec = s.registry.get("local:rq-demo")!!
        assertEquals(listOf("rq_demo"), rec.schemaIds)
        assertEquals("local", rec.source)
    }

    @Test
    fun `匯入惡意 zip 整包拒絕且使用者目錄一片空白`() {
        val (s, user, _) = store()
        val r = s.importLocal(StoreFixtures.malicious("evil-traversal.zip"), "evil.zip") {}
        assertTrue(r is SchemaStore.Outcome.Failed)
        val failed = r as SchemaStore.Outcome.Failed
        // ⚠ 比對的是**資源 id**，不是譯文。訊息現在是 UiMessage（id + 參數），
        // 比對譯文等於把這條測試綁死在中文上 —— 而重點正好是它不該是中文。
        assertEquals(R.string.store_err_import_rejected, failed.message.id)
        assertEquals("被拒絕的檔名要帶到訊息裡", listOf<Any>("evil.zip"), failed.message.args)
        assertTrue(
            "拒絕理由要指名是路徑問題，否則使用者不知道自己收到的是什麼",
            failed.details.any { it.id == R.string.archive_err_path },
        )
        assertEquals(0, user.listFiles()?.size ?: 0)
        assertFalse(File(user.parentFile, "evil.yaml").exists())
    }

    @Test
    fun `單一 yaml 匯入接受，可執行檔不接受`() {
        val (s, user, _) = store()
        val yaml = File(tmp.root, "my.schema.yaml").apply { writeText("schema:\n  schema_id: my\n") }
        assertTrue(s.importLocal(yaml, "my.schema.yaml") {} is SchemaStore.Outcome.Ok)
        assertTrue(File(user, "my.schema.yaml").isFile)

        val exe = File(tmp.root, "x.so").apply { writeText("ELF") }
        assertTrue(s.importLocal(exe, "x.so") {} is SchemaStore.Outcome.Failed)
    }

    @Test
    fun `單一檔案的檔名也要過路徑檢查`() {
        val (s, user, _) = store()
        val yaml = File(tmp.root, "ok.yaml").apply { writeText("a: 1\n") }
        val r = s.importLocal(yaml, "../../escape.yaml") {}
        assertTrue(r is SchemaStore.Outcome.Failed)
        assertFalse(File(user.parentFile.parentFile, "escape.yaml").exists())
    }
}
