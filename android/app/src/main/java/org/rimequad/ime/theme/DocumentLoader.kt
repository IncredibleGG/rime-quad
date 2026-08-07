package org.rimequad.ime.theme

/**
 * 依 id 取得 YAML 原文。搜尋路徑順序（使用者目錄 → 隨附目錄 → 內建）由實作負責，
 * 見 docs/theme-format.md §2.3。找不到回傳 null。
 */
fun interface DocumentSource {
    fun read(id: String): String?
}

/** 以記憶體 Map 為後端的來源，供單元測試與內建主題使用。 */
class MapDocumentSource(private val docs: Map<String, String>) : DocumentSource {
    override fun read(id: String): String? = docs[id]
}

/** 多來源串接，先找到者勝出（§2.3）。 */
class ChainedDocumentSource(private val sources: List<DocumentSource>) : DocumentSource {
    override fun read(id: String): String? {
        for (s in sources) {
            val t = s.read(id)
            if (t != null) return t
        }
        return null
    }
}

/** §7.2 的合併規則。序列永不逐元素合併；顯式 null 代表刪除。 */
object NodeMerge {

    fun mergeMapping(base: YamlNode.Mapping, over: YamlNode.Mapping): YamlNode.Mapping {
        val out = LinkedHashMap<String, YamlNode>(base.entries)
        for ((k, v) in over.entries) {
            if (v is YamlNode.Scalar && v.value == null) {
                out.remove(k)
                continue
            }
            val b = out[k]
            out[k] = if (b is YamlNode.Mapping && v is YamlNode.Mapping) mergeMapping(b, v) else v
        }
        return YamlNode.Mapping(out, base.line)
    }
}

class MergedDocument(val root: YamlNode.Mapping, val ancestry: List<String>)

/**
 * 規範 §7.4 的第 1–5 步：讀取、標頭檢查、繼承鏈、深度合併、平台覆寫。
 * 回傳 null 表示致命錯誤（診斷已寫入 [Diagnostics]）。
 */
object DocumentLoader {

    const val CLIENT_VERSION = "0.1.0"
    private const val MAX_CHAIN = 8

    /** §7.3：永遠取自最終文件本身，不從父代繼承。 */
    private val OWN_KEYS = listOf("format", "id", "revision", "inherits", "min_client")

    private val ID_PATTERN = Regex("[a-z0-9][a-z0-9._-]{0,63}")

    fun load(
        kind: String,
        supportedMajor: Int,
        id: String,
        source: DocumentSource,
        platform: Platform,
        diag: Diagnostics,
        clientVersion: String
    ): MergedDocument? {
        val docs = ArrayList<YamlNode.Mapping>()
        val ids = ArrayList<String>()
        val seen = LinkedHashSet<String>()
        var currentId = id

        while (true) {
            if (!seen.add(currentId)) {
                diag.error(
                    "inherits",
                    "F6: inheritance cycle: ${seen.joinToString(" -> ")} -> $currentId"
                )
                return null
            }
            if (docs.size >= MAX_CHAIN) {
                diag.error("inherits", "F6: inheritance chain is deeper than $MAX_CHAIN documents")
                return null
            }

            val text = source.read(currentId)
            if (text == null) {
                if (docs.isEmpty()) diag.error("", "document '$currentId' was not found in any search path")
                else diag.error("inherits", "F5: parent document '$currentId' was not found")
                return null
            }

            val parsed = try {
                MiniYaml.parse("$currentId.yaml", text)
            } catch (e: YamlSyntaxException) {
                diag.error("", "F1: ${e.detail}", e.line)
                return null
            }
            for (w in parsed.warnings) diag.warn("", w.message, w.line)

            val root = parsed.root as? YamlNode.Mapping
            if (root == null) {
                diag.error("", "F2: the document root must be a mapping")
                return null
            }
            if (!checkHeader(root, kind, supportedMajor, currentId, diag)) return null
            if (docs.isEmpty() && !checkMinClient(root, clientVersion, diag)) return null

            docs.add(root)
            ids.add(currentId)

            val parent = (root.entries["inherits"] as? YamlNode.Scalar)?.value ?: break
            currentId = parent
        }

        // 自最遠祖先向下合併。
        var merged = docs[docs.size - 1]
        for (i in docs.size - 2 downTo 0) merged = NodeMerge.mergeMapping(merged, docs[i])
        merged = restoreOwnKeys(merged, docs[0])
        merged = applyPlatformOverrides(merged, platform, diag)

        return MergedDocument(merged, ids.reversed())
    }

    private fun checkHeader(
        root: YamlNode.Mapping,
        kind: String,
        supportedMajor: Int,
        id: String,
        diag: Diagnostics
    ): Boolean {
        val fmt = (root.entries["format"] as? YamlNode.Scalar)?.value
        if (fmt == null) {
            diag.error("format", "F3: '$id' is missing the required 'format' field", root.line)
            return false
        }
        val idx = fmt.lastIndexOf('/')
        if (idx <= 0 || idx == fmt.length - 1) {
            diag.error("format", "F3: malformed format tag '$fmt' in '$id'", root.line)
            return false
        }
        val docKind = fmt.substring(0, idx)
        val major = fmt.substring(idx + 1).toIntOrNull()
        if (docKind != kind) {
            diag.error("format", "F3: '$id' is a '$docKind' document but a '$kind' was expected", root.line)
            return false
        }
        if (major == null || major < 1) {
            diag.error("format", "F3: malformed major version in '$fmt'", root.line)
            return false
        }
        if (major > supportedMajor) {
            diag.error(
                "format",
                "F3: '$id' uses $kind format v$major; this client supports up to v$supportedMajor. " +
                    "Please update the app.",
                root.line
            )
            return false
        }

        val docId = (root.entries["id"] as? YamlNode.Scalar)?.value
        if (docId == null) {
            diag.error("id", "F4: '$id' is missing the required 'id' field", root.line)
            return false
        }
        if (!ID_PATTERN.matches(docId)) {
            diag.error("id", "F4: '$docId' is not a valid id (lowercase [a-z0-9._-], 1-64 chars)", root.line)
            return false
        }
        if (docId != id) {
            diag.error("id", "F4: id '$docId' does not match the document name '$id'", root.line)
            return false
        }
        return true
    }

    private fun checkMinClient(root: YamlNode.Mapping, clientVersion: String, diag: Diagnostics): Boolean {
        val min = (root.entries["min_client"] as? YamlNode.Scalar)?.value ?: return true
        if (compareVersions(clientVersion, min) < 0) {
            diag.error(
                "min_client",
                "F7: this document requires client $min or newer (running $clientVersion)",
                root.line
            )
            return false
        }
        return true
    }

    internal fun compareVersions(a: String, b: String): Int {
        val pa = a.split(".")
        val pb = b.split(".")
        val n = if (pa.size > pb.size) pa.size else pb.size
        for (i in 0 until n) {
            val va = pa.getOrNull(i)?.trim()?.toIntOrNull() ?: 0
            val vb = pb.getOrNull(i)?.trim()?.toIntOrNull() ?: 0
            if (va != vb) return if (va < vb) -1 else 1
        }
        return 0
    }

    private fun restoreOwnKeys(merged: YamlNode.Mapping, leaf: YamlNode.Mapping): YamlNode.Mapping {
        val out = LinkedHashMap(merged.entries)
        for (k in OWN_KEYS) {
            val v = leaf.entries[k]
            if (v == null) out.remove(k) else out[k] = v
        }
        return YamlNode.Mapping(out, merged.line)
    }

    private fun applyPlatformOverrides(
        merged: YamlNode.Mapping,
        platform: Platform,
        diag: Diagnostics
    ): YamlNode.Mapping {
        val po = merged.entries["platform_overrides"] as? YamlNode.Mapping ?: return merged
        // §7.4：未知平台鍵必須被忽略且不產生 WARNING（新增平台時的前向相容機制）。
        val branch = po.entries[platform.key] as? YamlNode.Mapping ?: return merged
        val cleaned = LinkedHashMap<String, YamlNode>()
        for ((k, v) in branch.entries) {
            if (k == "platform_overrides") {
                diag.warn(
                    "platform_overrides.${platform.key}.platform_overrides",
                    "platform overrides must not be nested; ignored",
                    v.line
                )
                continue
            }
            cleaned[k] = v
        }
        if (cleaned.isEmpty()) return merged
        return NodeMerge.mergeMapping(merged, YamlNode.Mapping(cleaned, branch.line))
    }
}

/** 主題載入的公開進入點。 */
object ThemeLoader {

    const val SUPPORTED_MAJOR = 1
    const val KIND = "rime-theme"

    fun load(
        id: String,
        source: DocumentSource,
        platform: Platform = Platform.ANDROID,
        locale: String = "en",
        clientVersion: String = DocumentLoader.CLIENT_VERSION
    ): LoadResult<Theme> {
        val diag = Diagnostics()
        val merged = DocumentLoader.load(KIND, SUPPORTED_MAJOR, id, source, platform, diag, clientVersion)
            ?: return LoadResult(null, diag.snapshot())
        val ctx = ParseContext(diag)
        ctx.locale = locale
        val theme = ThemeParser.bind(merged.root, id, merged.ancestry, ctx)
        return LoadResult(theme, diag.snapshot())
    }
}

/** 佈局載入的公開進入點。僅行動端使用。 */
object LayoutLoader {

    const val SUPPORTED_MAJOR = 1
    const val KIND = "rime-layout"

    fun load(
        id: String,
        source: DocumentSource,
        platform: Platform = Platform.ANDROID,
        locale: String = "en",
        clientVersion: String = DocumentLoader.CLIENT_VERSION
    ): LoadResult<KeyboardLayout> {
        val diag = Diagnostics()
        val merged = DocumentLoader.load(KIND, SUPPORTED_MAJOR, id, source, platform, diag, clientVersion)
            ?: return LoadResult(null, diag.snapshot())
        val patched = applyKeyPatches(merged.root, diag)
        val ctx = ParseContext(diag)
        ctx.locale = locale
        val layout = LayoutParser.bind(patched, id, merged.ancestry, ctx)
        return LoadResult(layout, diag.snapshot())
    }

    /**
     * §9.7：把 `key_patches` 套用到所有 layer 中 id 相符的鍵。
     * 在節點層做，因此自動沿用 §7.2 的合併語義（含 null 刪除）。
     */
    internal fun applyKeyPatches(root: YamlNode.Mapping, diag: Diagnostics): YamlNode.Mapping {
        val patches = root.entries["key_patches"] as? YamlNode.Mapping ?: return root
        if (patches.entries.isEmpty()) return root
        val layers = root.entries["layers"] as? YamlNode.Sequence ?: return root

        val applied = HashSet<String>()
        val newLayers = ArrayList<YamlNode>(layers.items.size)
        for (layerNode in layers.items) newLayers.add(patchLayer(layerNode, patches.entries, applied))

        for (k in patches.entries.keys) {
            if (!applied.contains(k)) {
                diag.warn("key_patches.$k", "no key with id '$k' exists in this layout; patch ignored")
            }
        }

        val out = LinkedHashMap(root.entries)
        out["layers"] = YamlNode.Sequence(newLayers, layers.line)
        return YamlNode.Mapping(out, root.line)
    }

    private fun patchLayer(
        layerNode: YamlNode,
        patches: Map<String, YamlNode>,
        applied: MutableSet<String>
    ): YamlNode {
        val lm = layerNode as? YamlNode.Mapping ?: return layerNode
        val rows = lm.entries["rows"] as? YamlNode.Sequence ?: return layerNode
        val newRows = ArrayList<YamlNode>(rows.items.size)
        for (rowNode in rows.items) newRows.add(patchRow(rowNode, patches, applied))
        val out = LinkedHashMap(lm.entries)
        out["rows"] = YamlNode.Sequence(newRows, rows.line)
        return YamlNode.Mapping(out, lm.line)
    }

    private fun patchRow(
        rowNode: YamlNode,
        patches: Map<String, YamlNode>,
        applied: MutableSet<String>
    ): YamlNode {
        val rm = rowNode as? YamlNode.Mapping ?: return rowNode
        val keys = rm.entries["keys"] as? YamlNode.Sequence ?: return rowNode
        val newKeys = ArrayList<YamlNode>(keys.items.size)
        for (keyNode in keys.items) newKeys.add(patchKey(keyNode, patches, applied))
        val out = LinkedHashMap(rm.entries)
        out["keys"] = YamlNode.Sequence(newKeys, keys.line)
        return YamlNode.Mapping(out, rm.line)
    }

    private fun patchKey(
        keyNode: YamlNode,
        patches: Map<String, YamlNode>,
        applied: MutableSet<String>
    ): YamlNode {
        val km = keyNode as? YamlNode.Mapping ?: return keyNode
        val kid = (km.entries["id"] as? YamlNode.Scalar)?.value ?: return keyNode
        val patch = patches[kid] as? YamlNode.Mapping ?: return keyNode
        applied.add(kid)
        return NodeMerge.mergeMapping(km, patch)
    }
}
