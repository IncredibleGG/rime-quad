package org.rimequad.ime.store

import org.rimequad.ime.theme.MiniYaml
import org.rimequad.ime.theme.YamlNode
import java.io.File

/**
 * `default.custom.yaml` 的 `patch/schema_list` 讀寫（規範 §3）。
 *
 * ── 為什麼是文字手術，不是「解析後重新輸出」──────────────────────────
 * MiniYaml 是**讀取器**，沒有 round-trip 能力：它丟掉註解、丟掉引號風格、
 * 丟掉區塊順序。用它讀進來再輸出，會把使用者自己加的設定與 collect_data.sh
 * 產生的說明註解全部洗掉。所以：
 *   · **讀**用 MiniYaml（要正確理解 YAML）
 *   · **寫**只動 `schema_list:` 底下那幾行（不能傷及其他內容）
 *
 * ── 回滾為什麼是「整份還原」──────────────────────────────────────────
 * 規範 §3 要求部署失敗時把 schema_list 還原成導入前的狀態。最可靠的
 * 還原不是「反向再做一次文字手術」（那要求手術本身沒有 bug），
 * 而是把導入前的**整份檔案位元組**存起來、失敗時原樣寫回。
 * 見 [Snapshot]。
 */
object SchemaListPatch {

    const val FILE_NAME = "default.custom.yaml"

    /** 導入前的整份檔案內容。null 代表當時檔案不存在（回滾時要刪掉它）。 */
    data class Snapshot(val text: String?)

    fun file(userDataDir: File): File = File(userDataDir, FILE_NAME)

    fun snapshot(userDataDir: File): Snapshot {
        val f = file(userDataDir)
        return Snapshot(if (f.isFile) f.readText(Charsets.UTF_8) else null)
    }

    fun restore(userDataDir: File, snapshot: Snapshot) {
        val f = file(userDataDir)
        val text = snapshot.text
        if (text == null) f.delete() else f.writeText(text, Charsets.UTF_8)
    }

    /** 目前已啟用的方案 id，依 schema_list 的順序。檔案不存在時回空清單。 */
    fun read(userDataDir: File): List<String> {
        val f = file(userDataDir)
        if (!f.isFile) return emptyList()
        return readFrom(runCatching { f.readText(Charsets.UTF_8) }.getOrNull() ?: return emptyList())
    }

    internal fun readFrom(text: String): List<String> {
        val doc = runCatching { MiniYaml.parse(FILE_NAME, text) }.getOrNull() ?: return emptyList()
        val root = doc.root as? YamlNode.Mapping ?: return emptyList()
        val patch = root.entries["patch"] as? YamlNode.Mapping ?: return emptyList()
        val list = patch.entries["schema_list"] as? YamlNode.Sequence ?: return emptyList()
        return list.items.mapNotNull { item ->
            when (item) {
                // - schema: luna_pinyin
                is YamlNode.Mapping -> (item.entries["schema"] as? YamlNode.Scalar)?.value
                // - luna_pinyin   （上游偶爾這樣寫，讀得進來比較保險）
                is YamlNode.Scalar -> item.value
                else -> null
            }
        }.filter { it.isNotEmpty() }
    }

    fun write(userDataDir: File, ids: List<String>) {
        val f = file(userDataDir)
        val original = if (f.isFile) f.readText(Charsets.UTF_8) else null
        f.parentFile?.mkdirs()
        f.writeText(writeInto(original, ids), Charsets.UTF_8)
    }

    /**
     * 把 [ids] 寫成 `patch/schema_list`，其餘內容原封不動。
     *
     * 三種情形：
     *   1. 已有 `schema_list:` → 換掉它底下的項目行
     *   2. 有 `patch:` 沒有 `schema_list:` → 在 patch 底下插入
     *   3. 兩者都沒有（含空檔案）→ 附加一整個 patch 區塊
     */
    internal fun writeInto(original: String?, ids: List<String>): String {
        val text = original ?: ""
        // 沿用既有那一行的原文，行末註解才不會被洗掉。
        // 隨附的 default.custom.yaml 長這樣：
        //     - schema: luna_pinyin_tw    # 拼音（臺灣字形）
        // 若一律重新產生成 `- schema: luna_pinyin_tw`，使用者只是裝了一個
        // 方案，卻發現自己檔案裡的說明全沒了。
        val existing = existingItemLines(text)
        val body = ids.joinToString("\n") { existing[it] ?: "    - schema: $it" }
        val lines = text.split("\n")

        var schemaListIdx = -1
        var schemaListIndent = 0
        var patchIdx = -1
        var inPatch = false
        for ((i, raw) in lines.withIndex()) {
            val indent = raw.indexOfFirst { it != ' ' }.let { if (it < 0) raw.length else it }
            val content = raw.trim()
            if (content.isEmpty() || content.startsWith("#")) continue
            if (indent == 0) inPatch = false
            if (indent == 0 && (content == "patch:" || content.startsWith("patch:"))) {
                patchIdx = i
                inPatch = true
                continue
            }
            if (inPatch && content.removeSuffix(":").trim() == "schema_list" &&
                content.endsWith(":")
            ) {
                schemaListIdx = i
                schemaListIndent = indent
                break
            }
        }

        if (schemaListIdx >= 0) {
            // 吃掉所有縮排比 schema_list 深的後續行（就是它的項目）。
            var end = schemaListIdx + 1
            while (end < lines.size) {
                val raw = lines[end]
                if (raw.isBlank()) { end++; continue }
                val indent = raw.indexOfFirst { it != ' ' }.let { if (it < 0) raw.length else it }
                if (indent <= schemaListIndent) break
                end++
            }
            val head = lines.subList(0, schemaListIdx + 1)
            val tail = lines.subList(end, lines.size)
            return (head + body.split("\n").filter { it.isNotEmpty() } + tail)
                .joinToString("\n")
                .trimEnd() + "\n"
        }

        if (patchIdx >= 0) {
            val head = lines.subList(0, patchIdx + 1)
            val tail = lines.subList(patchIdx + 1, lines.size)
            return (head + listOf("  schema_list:") +
                body.split("\n").filter { it.isNotEmpty() } + tail)
                .joinToString("\n").trimEnd() + "\n"
        }

        val prefix = if (text.isBlank()) "" else text.trimEnd() + "\n\n"
        return prefix + "patch:\n  schema_list:\n" +
            body.split("\n").filter { it.isNotEmpty() }.joinToString("\n") + "\n"
    }

    /**
     * 掃出目前 schema_list 底下每個 id 對應的**原始那一行**（含行末註解）。
     * 只認 `- schema: <id>` 與 `- <id>` 兩種寫法，其餘忽略。
     */
    private fun existingItemLines(text: String): Map<String, String> {
        val out = HashMap<String, String>()
        val re = Regex("^\\s*-\\s*(?:schema:\\s*)?([A-Za-z0-9_.\\-]+)\\s*(?:#.*)?$")
        for (line in text.split("\n")) {
            val id = re.find(line)?.groupValues?.get(1) ?: continue
            out.putIfAbsent(id, line.trimEnd())
        }
        return out
    }

    /** 加入（去重、保序、加在最後）。回傳實際新增的 id。 */
    fun enable(userDataDir: File, ids: List<String>): List<String> {
        val current = read(userDataDir)
        val added = ids.filter { it.isNotEmpty() && it !in current }
        if (added.isEmpty()) return emptyList()
        write(userDataDir, current + added)
        return added
    }

    /** 移除。回傳實際移除的 id。 */
    fun disable(userDataDir: File, ids: List<String>): List<String> {
        val current = read(userDataDir)
        val removed = ids.filter { it in current }
        if (removed.isEmpty()) return emptyList()
        write(userDataDir, current.filterNot { it in ids })
        return removed
    }
}
