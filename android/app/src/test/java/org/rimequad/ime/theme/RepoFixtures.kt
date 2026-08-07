package org.rimequad.ime.theme

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

    val themeIds = listOf("default-light", "default-dark", "sakura-light", "sakura-dark")

    val layoutIds = listOf("qwerty", "numeric-symbol", "bopomofo-dachen", "t9-pinyin")

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
