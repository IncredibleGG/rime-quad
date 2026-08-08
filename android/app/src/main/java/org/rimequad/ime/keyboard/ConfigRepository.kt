package org.rimequad.ime.keyboard

import android.content.Context
import android.content.res.AssetManager
import android.util.Log
import org.rimequad.ime.DiagnosticStrings
import org.rimequad.ime.core.RimeRuntime
import org.rimequad.ime.theme.ChainedDocumentSource
import org.rimequad.ime.theme.Diagnostic
import org.rimequad.ime.theme.DiagnosticText
import org.rimequad.ime.theme.DocumentSource
import org.rimequad.ime.theme.KeyboardLayout
import org.rimequad.ime.theme.LayoutLoader
import org.rimequad.ime.theme.LoadResult
import org.rimequad.ime.theme.MapDocumentSource
import org.rimequad.ime.theme.Platform
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
class ConfigRepository(context: Context) : LayoutRepository {

    private val assets: AssetManager = context.applicationContext.assets

    init {
        // 方案 → 語言的對照表和佈局、主題一樣是隨 APK 出貨的宣告式資料，
        // 由同一條 Gradle syncRimeData 任務放進 assets。在這裡載入是因為
        // 這裡是唯一同時握有 AssetManager 又負責「讀隨附設定」的地方 ——
        // 鍵盤類型選單的分組要用它（見 [SchemaLanguages]）。
        SchemaLanguages.loadShipped(assets)

        // 診斷的在地化樣板。解析層（theme/）刻意一行 android.* 都沒有 —— iOS 端
        // 要照抄的就是它 —— 所以「code → 當地語言的字」這條線在這裡接上。
        // App 與輸入法各自都會建一個 ConfigRepository，兩邊都會走到。
        DiagnosticStrings.install(context)
    }

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
    override fun layoutIds(): List<String> = idsIn(LAYOUT_DIR)

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

    override fun loadLayout(id: String): LoadResult<KeyboardLayout> =
        LayoutLoader.load(id, layouts, Platform.ANDROID, locale = LOCALE)

    override fun loadTheme(id: String): LoadResult<Theme> =
        ThemeLoader.load(id, themes, Platform.ANDROID, locale = LOCALE)

    /**
     * 最後一道防線。§6.1 規定「不得顯示空白鍵盤」：即使隨附主題整份壞掉，
     * 附錄 A 的最小主題也保證載得起來，其餘一切走規範預設值。
     */
    override fun builtinFallbackTheme(): Theme =
        ThemeLoader.load("builtin-fallback", MapDocumentSource(mapOf(BUILTIN_ID to BUILTIN_THEME)))
            .value
            ?: error("附錄 A 的最小主題都載不起來，解析器壞了")

    companion object {
        private const val ASSET_ROOT = "rime"
        private const val LAYOUT_DIR = "layouts"
        private const val THEME_DIR = "themes"
        private const val SUFFIX = ".yaml"

        /**
         * §4.9 的在地化查詢語系。公開是因為 UI 側（鍵盤類型選單）也要用同一個
         * 值解析佈局名 —— 兩處各寫一份字面常數，遲早會變成兩種語言。
         *
         * ⚠ 這裡**不是常數**，是每次讀取都問一次系統。原本寫死 `"zh-Hant-TW"`，
         * 於是一個英文使用者在「挑一個鍵盤」那一屏上會看到「九宮格拼音」——
         * 介面其他每個字都翻了，只有這幾個從 yaml 讀出來的名字沒翻。
         *
         * 佈局／主題 yaml 的 `name` 本來就是 §4.9 的在地化字串（`en` / `zh-Hant`
         * / `zh-Hans` 各一份），[org.rimequad.ime.theme.LocalizedString.get] 也
         * 早就實作了 BCP 47 的回落鏈。缺的只是「拿使用者的語系去查」這一步。
         *
         * ⚠ 中間那一步 `addLikelySubtags` 不能省。使用者的語系常常是
         * `zh-TW`（沒有書寫系統那一段），而 yaml 裡的鍵是 `zh-Hant` / `zh-Hans`。
         * 直接拿 `zh-TW` 去查，回落鏈會一路退到「語言相同就算數」那一條，
         * 然後看 map 的順序決定回繁體還是簡體 —— 一個臺灣使用者會不會看到簡體
         * 取決於 yaml 裡哪一行寫在前面。ICU 的 likely-subtags 把 `zh-TW` 補成
         * `zh-Hant-TW`、`zh-CN` 補成 `zh-Hans-CN`，那條回落鏈才問得對。
         *
         * 補完之後給的是 `en-Latn-US` / `zh-Hant-TW` 這種完整標記，正好是回落鏈
         * 吃的格式。任何一步失敗就退回英文 —— 與 `values/strings.xml` 是英文
         * 同一個理由。
         */
        val LOCALE: String
            get() = runCatching {
                val l = java.util.Locale.getDefault()
                runCatching {
                    android.icu.util.ULocale.addLikelySubtags(android.icu.util.ULocale.forLocale(l))
                        .toLanguageTag()
                }.getOrNull()?.takeIf { it.isNotBlank() } ?: l.toLanguageTag()
            }.getOrNull()?.takeIf { it.isNotBlank() && it != "und" } ?: "en"

        private const val BUILTIN_ID = "builtin-fallback"
        private val BUILTIN_THEME = """
            format: rime-theme/1
            id: builtin-fallback
            appearance: light
            name: "Fallback"
        """.trimIndent()

        /**
         * 給人看的診斷。**不要用 [Diagnostic.toString]** —— 那是開發者回退
         * （`[WARNING] path bad_color(#ZZZ)`），不是使用者該看到的東西。
         */
        fun describe(diagnostics: List<Diagnostic>): List<String> =
            DiagnosticText.renderAll(diagnostics)
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
