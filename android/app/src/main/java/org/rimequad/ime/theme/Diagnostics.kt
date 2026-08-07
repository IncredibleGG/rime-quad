package org.rimequad.ime.theme

/** 診斷嚴重度。ERROR 只用於致命錯誤（docs/theme-format.md §6.2）。 */
enum class Severity { INFO, WARNING, ERROR }

/**
 * 一則診斷。四端必須都能產生等價的清單 —— 同一份壞檔案，四端應報一樣多則。
 *
 * @param path YAML 路徑，如 `keyboard.key_styles.default.background`
 * @param line 來源行號；讀取層無法提供時為 null
 */
data class Diagnostic(
    val severity: Severity,
    val path: String,
    val message: String,
    val line: Int? = null
) {
    override fun toString(): String {
        val where = if (line != null) ":$line" else ""
        val at = if (path.isEmpty()) "<document>" else path
        return "[$severity] $at$where — $message"
    }
}

class Diagnostics {
    private val list = ArrayList<Diagnostic>()

    val items: List<Diagnostic> get() = list

    val hasErrors: Boolean get() = list.any { it.severity == Severity.ERROR }

    val warningCount: Int get() = list.count { it.severity == Severity.WARNING }

    fun info(path: String, message: String, line: Int? = null) {
        list.add(Diagnostic(Severity.INFO, path, message, line))
    }

    fun warn(path: String, message: String, line: Int? = null) {
        list.add(Diagnostic(Severity.WARNING, path, message, line))
    }

    fun error(path: String, message: String, line: Int? = null) {
        list.add(Diagnostic(Severity.ERROR, path, message, line))
    }

    fun addAll(other: Diagnostics) {
        list.addAll(other.list)
    }

    fun snapshot(): List<Diagnostic> = ArrayList(list)
}

/** 載入結果。[value] 為 null 表示致命錯誤，呼叫端必須退回上一個成功的主題。 */
class LoadResult<T>(val value: T?, val diagnostics: List<Diagnostic>) {
    val isSuccess: Boolean get() = value != null
    val errors: List<Diagnostic> get() = diagnostics.filter { it.severity == Severity.ERROR }
    val warnings: List<Diagnostic> get() = diagnostics.filter { it.severity == Severity.WARNING }
}

/** 目標平台。決定 `platform_overrides` 取哪一個分支。 */
enum class Platform(val key: String) {
    ANDROID("android"),
    IOS("ios"),
    MACOS("macos"),
    WINDOWS("windows");

    companion object {
        val ALL_KEYS: Set<String> = values().map { it.key }.toSet()
    }
}
