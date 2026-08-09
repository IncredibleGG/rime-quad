package org.luminakey.ime.theme

import java.io.File

/**
 * 測試夾具：直接讀 repo 的 `core/themes` 與 `core/layouts`。
 *
 * 刻意**不**複製一份到 test resources —— 這些測試的價值就在於守住
 * 「真正會被四端載入的那幾份檔案」。複製品會腐爛。
 *
 * 工作目錄在 Gradle 下是模組目錄（android/app），因此向上尋找含 `core/` 的目錄。
 */
object RepoFixtures {

    val coreDir: File by lazy { findCore() }

    val themes: DocumentSource by lazy { dirSource(File(coreDir, "themes")) }

    val layouts: DocumentSource by lazy { dirSource(File(coreDir, "layouts")) }

    /**
     * `core/themes` 底下**每一份**主題，理由與 [layoutIds] 完全相同。
     *
     * 這裡一度也是手寫的四個 id，於是十二份主題有八份從來沒有被
     * [org.luminakey.ime.theme.ThemeParserTest] 或 [MiniYamlTest] 載入過 ——
     * 佈局那一份清單踩過的坑，主題這一份原封不動地留著。
     * [org.luminakey.ime.keyboard.DeadKeyTest] 當時只好自己再掃一次目錄，
     * 那正是「同一件事有兩份實作」的開端。
     */
    val themeIds: List<String> by lazy {
        File(coreDir, "themes").listFiles().orEmpty()
            .filter { it.isFile && it.name.endsWith(".yaml") }
            .map { it.name.removeSuffix(".yaml") }
            .sorted()
    }

    /**
     * `core/layouts` 底下**每一份**佈局，不是手寫的白名單。
     *
     * 原本這裡寫死四個 id，於是 [org.luminakey.ime.keyboard.LayoutEscapeTest]
     * 只走得到四份 —— 十二份佈局裡有八份（含華語線新加的 `cn-t9-pinyin`、
     * `cn-symbols`）從來沒有被「進得去出得來」檢查過。新增一份佈局卻忘記
     * 更新這份清單，正是最容易讓死路溜進去的方式，所以改成掃目錄。
     *
     * 排序固定，讓 §9.1.1「取搜尋路徑裡最先找到的那一份」在測試裡是決定性的。
     */
    val layoutIds: List<String> by lazy {
        File(coreDir, "layouts").listFiles().orEmpty()
            .filter { it.isFile && it.name.endsWith(".yaml") }
            .map { it.name.removeSuffix(".yaml") }
            .sorted()
    }

    private fun findCore(): File {
        var dir: File? = File("").absoluteFile
        var depth = 0
        while (dir != null && depth < 10) {
            val candidate = File(dir, "core")
            if (File(candidate, "themes/default-light.yaml").isFile) return candidate
            dir = dir.parentFile
            depth++
        }
        throw IllegalStateException(
            "cannot locate <repo>/core starting from " + File("").absolutePath
        )
    }

    private fun dirSource(dir: File): DocumentSource = DocumentSource { id ->
        val f = File(dir, "$id.yaml")
        if (f.isFile) f.readText(Charsets.UTF_8) else null
    }

    fun describe(diags: List<Diagnostic>): String =
        if (diags.isEmpty()) "<none>" else diags.joinToString("\n  ", prefix = "\n  ")
}
