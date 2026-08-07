package org.rimequad.ime.keyboard

import android.content.Context
import android.content.res.AssetManager
import android.util.Log
import org.rimequad.ime.core.RimeRuntime
import org.rimequad.ime.theme.ChainedDocumentSource
import org.rimequad.ime.theme.Diagnostic
import org.rimequad.ime.theme.DocumentSource
import org.rimequad.ime.theme.KeyboardLayout
import org.rimequad.ime.theme.LayoutLoader
import org.rimequad.ime.theme.LoadResult
import org.rimequad.ime.theme.MapDocumentSource
import org.rimequad.ime.theme.Platform
import org.rimequad.ime.theme.Severity
import org.rimequad.ime.theme.Theme
import org.rimequad.ime.theme.ThemeLoader
import java.io.File
import java.io.IOException

/**
 * 主題與佈局的來源。實作 docs/theme-format.md §2.3 的搜尋路徑：
 *
 *   1. `<user_data_dir>/themes|layouts/`   使用者自己放的
 *   2. `<shared_data_dir>/themes|layouts/` 隨附資料目錄（解壓後的）
 *   3. 內建資源                            APK assets 內的 `rime/themes|layouts`
 *
 * 第 3 層的內容由 Gradle 的 `syncRimeData` 從 repo 的 `core/layouts`、
 * `core/themes` 同步而來 —— **沒有第二份副本**，那幾個 yaml 就是四端共用的
 * 唯一真相。
 *
 * §2.4 的「主題套件」目錄形態（`sakura-dark/sakura-dark.yaml`）也支援，
 * 單檔形態優先。
 */
class ConfigRepository(context: Context) {

    private val assets: AssetManager = context.applicationContext.assets

    val layouts: DocumentSource by lazy { sourcesFor(LAYOUT_DIR) }
    val themes: DocumentSource by lazy { sourcesFor(THEME_DIR) }

    private fun sourcesFor(kind: String): DocumentSource = ChainedDocumentSource(
        listOfNotNull(
            RimeRuntime.userDirOrNull?.let { FileDocumentSource(File(it, kind)) },
            RimeRuntime.sharedDirOrNull?.let { FileDocumentSource(File(it, kind)) },
            AssetDocumentSource(assets, "$ASSET_ROOT/$kind"),
        )
    )

    /** 目前可見的所有佈局 id（三層搜尋路徑聯集，先出現者勝出）。 */
    fun layoutIds(): List<String> = idsIn(LAYOUT_DIR)

    fun themeIds(): List<String> = idsIn(THEME_DIR)

    private fun idsIn(kind: String): List<String> {
        val out = LinkedHashSet<String>()
        listOfNotNull(RimeRuntime.userDirOrNull, RimeRuntime.sharedDirOrNull).forEach { root ->
            File(root, kind).listFiles()?.forEach { f ->
                if (f.isFile && f.name.endsWith(SUFFIX)) out.add(f.name.removeSuffix(SUFFIX))
                if (f.isDirectory && File(f, f.name + SUFFIX).isFile) out.add(f.name)
            }
        }
        runCatching { assets.list("$ASSET_ROOT/$kind") }.getOrNull()?.forEach { name ->
            if (name.endsWith(SUFFIX)) out.add(name.removeSuffix(SUFFIX))
        }
        return out.toList()
    }

    fun loadLayout(id: String): LoadResult<KeyboardLayout> =
        LayoutLoader.load(id, layouts, Platform.ANDROID, locale = LOCALE)

    fun loadTheme(id: String): LoadResult<Theme> =
        ThemeLoader.load(id, themes, Platform.ANDROID, locale = LOCALE)

    /**
     * 最後一道防線。§6.1 規定「不得顯示空白鍵盤」：即使隨附主題整份壞掉，
     * 附錄 A 的最小主題也保證載得起來，其餘一切走規範預設值。
     */
    fun builtinFallbackTheme(): Theme =
        ThemeLoader.load("builtin-fallback", MapDocumentSource(mapOf(BUILTIN_ID to BUILTIN_THEME)))
            .value
            ?: error("附錄 A 的最小主題都載不起來，解析器壞了")

    companion object {
        private const val ASSET_ROOT = "rime"
        private const val LAYOUT_DIR = "layouts"
        private const val THEME_DIR = "themes"
        private const val SUFFIX = ".yaml"
        private const val LOCALE = "zh-Hant-TW"

        private const val BUILTIN_ID = "builtin-fallback"
        private val BUILTIN_THEME = """
            format: rime-theme/1
            id: builtin-fallback
            appearance: light
            name: "Fallback"
        """.trimIndent()

        fun describe(diagnostics: List<Diagnostic>): List<String> =
            diagnostics.filter { it.severity != Severity.INFO }.map { it.toString() }
    }
}

/** 檔案系統來源。目錄不存在時一律回 null，不當成錯誤。 */
private class FileDocumentSource(private val dir: File) : DocumentSource {
    override fun read(id: String): String? {
        val flat = File(dir, "$id.yaml")
        val packaged = File(File(dir, id), "$id.yaml")
        val f = if (flat.isFile) flat else if (packaged.isFile) packaged else return null
        return runCatching { f.readText(Charsets.UTF_8) }.getOrNull()?.let(::stripBom)
    }
}

/** APK assets 來源。 */
private class AssetDocumentSource(
    private val assets: AssetManager,
    private val dir: String,
) : DocumentSource {
    override fun read(id: String): String? {
        for (path in listOf("$dir/$id.yaml", "$dir/$id/$id.yaml")) {
            try {
                return assets.open(path).use { stripBom(String(it.readBytes(), Charsets.UTF_8)) }
            } catch (_: IOException) {
                // 換下一個候選路徑
            }
        }
        Log.d("ConfigRepository", "assets 內找不到 $dir/$id.yaml")
        return null
    }
}

/** §2.1：解析器遇到 BOM 必須略過它而非報錯。 */
private fun stripBom(text: String): String =
    if (text.isNotEmpty() && text[0] == '\uFEFF') text.substring(1) else text
