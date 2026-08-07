package org.rimequad.ime.store

import java.io.File

/**
 * 測試夾具：直接讀 `android/testdata/store` 底下由
 * `android/testdata/make_fake_store.py` 產生的假索引與惡意 zip。
 *
 * 刻意**不**複製一份到 test resources —— 同一批檔案還要餵給模擬器上的
 * HTTP server 做端到端驗證，兩份副本必然會分岔。與 theme 那條線的
 * [org.rimequad.ime.theme.RepoFixtures] 同一個理由。
 */
object StoreFixtures {

    val storeDir: File by lazy { findStore() }

    val indexJson: File get() = File(storeDir, "index.json")

    val indexText: String get() = indexJson.readText(Charsets.UTF_8)

    /**
     * 檔名形如 `<id>-<commit>.zip`。比對必須釘住 commit 那一段，
     * 否則 `rq-demo` 會誤中 `rq-demo-base-*.zip` —— 這個坑已經踩過一次。
     */
    fun packageZip(id: String): File {
        val re = Regex("^" + Regex.escape(id) + "-[0-9A-Za-z]+\\.zip$")
        return File(storeDir, "packages").listFiles()
            ?.firstOrNull { re.matches(it.name) }
            ?: error("找不到套件 $id；先跑 python3 android/testdata/make_fake_store.py --out android/testdata/store")
    }

    fun malicious(name: String): File =
        File(File(storeDir, "malicious"), name).also {
            check(it.isFile) { "找不到惡意 zip $name；先跑 make_fake_store.py" }
        }

    private fun findStore(): File {
        var dir: File? = File("").absoluteFile
        var depth = 0
        while (dir != null && depth < 10) {
            for (candidate in listOf(File(dir, "testdata/store"), File(dir, "android/testdata/store"))) {
                if (File(candidate, "index.json").isFile) return candidate
            }
            dir = dir.parentFile
            depth++
        }
        throw IllegalStateException(
            "找不到 android/testdata/store（起點 ${File("").absolutePath}）。" +
                "先跑 python3 android/testdata/make_fake_store.py --out android/testdata/store"
        )
    }
}
