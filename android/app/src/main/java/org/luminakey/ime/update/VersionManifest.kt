package org.luminakey.ime.update

import org.luminakey.ime.net.NetworkGate
import org.luminakey.ime.store.Json
import org.luminakey.ime.store.JsonSyntaxException
import org.luminakey.ime.store.MiniJson

/**
 * 應用內更新的版本資訊（`version.json`，由 `scripts/publish_apk.sh` 產生）。
 *
 * ── 這一層的安全性到哪為止（請不要誤會它擋得住什麼）────────────────────
 * `sha256` 與 APK **來自同一台伺服器**。伺服器被入侵時，攻擊者能同時換掉
 * 兩者，摘要照樣對得起來。所以 sha256 防的是**傳輸損壞**（半截的下載、
 * 壞掉的快取、中途斷線），不是伺服器被入侵。
 *
 * 真正擋住惡意替換的是 **Android 強制「升級必須同簽章」**：即使攻擊者把
 * R2 上的 APK 換成他自己的，那份 APK 沒有本專案的簽章金鑰（也沒有輪替
 * 證明），系統會直接拒絕安裝，使用者手上的輸入法動不了。
 *
 * 這就是為什麼「金鑰怎麼保管」是這條線的核心而不是附帶事項：
 * 更新機制的安全性幾乎全部押在那一把金鑰上。相關說明見 `rime-signing/README.md`。
 *
 * 要再往上一階（讓伺服器被入侵也擋得住），得用一把**離線**的金鑰對
 * version.json 本身簽名，並把公鑰內建進 APK。目前沒有做，別把它當有做。
 *
 * ── 為什麼比 version_code 而不是 version_name ──────────────────────────
 * `version_name` 是給人看的字串，格式隨時會改；拿字串比大小遲早出事
 * （"0.10.0" < "0.9.0" 這種）。`version_code` 是 Android 自己的升級語意
 * 所依據的整數，而且 [org.luminakey.ime.update.UpdateController] 拿到的
 * 「目前版本」也是同一個來源（PackageInfo），兩邊比的是同一件事。
 */
data class VersionManifest(
    /** 遠端版本的 versionCode。單調遞增，見 android/app/build.gradle.kts 的推導。 */
    val versionCode: Long,
    /** 給人看的版本字串。 */
    val versionName: String,
    /** 對應的 git commit（可能沒有）。 */
    val commit: String?,
    /** APK 檔名（可能沒有）。 */
    val fileName: String?,
    /** APK 位元組數。同時是下載的硬上限。 */
    val size: Long,
    /** APK 的 sha256（小寫十六進位 64 字元）。 */
    val sha256: String,
    /** 解析完成的絕對下載網址。 */
    val url: String,
    /** 更新說明。可能是空字串。 */
    val notes: String,
)

sealed class ManifestParseResult {
    data class Ok(val manifest: VersionManifest) : ManifestParseResult()
    data class Err(val message: String) : ManifestParseResult()
}

object VersionManifestParser {

    /** APK 再大也不該超過這個數。防的是「伺服器宣告一個荒謬的 size」。 */
    const val MAX_APK_BYTES = 300L * 1024 * 1024

    private val HEX64 = Regex("^[0-9a-f]{64}$")

    /**
     * 解析 version.json。
     *
     * 政策與索引那條線相反：**任何欄位不對就整份拒收**。索引可以「壞掉一個
     * 套件只丟掉那一個」，因為其餘的仍然有用；version.json 只描述一件事，
     * 其中任何一項不可信，整件事就不可信 —— 而這件事的後果是往使用者機器上
     * 裝一個 APK。
     *
     * @param manifestUrl version.json 自己的位置，用來解析相對網址。
     */
    fun parse(text: String, manifestUrl: String): ManifestParseResult {
        val root = try {
            MiniJson.parse(text)
        } catch (e: JsonSyntaxException) {
            return ManifestParseResult.Err("版本資訊不是合法的 JSON：${e.message}")
        }
        if (root !is Json.Obj) return ManifestParseResult.Err("版本資訊的頂層必須是物件")

        val versionCode = root.long("version_code")
            ?: return ManifestParseResult.Err(
                "版本資訊沒有 version_code，無從判斷新舊。" +
                    "（這是舊格式的 version.json，發布端更新後就會有。）"
            )
        if (versionCode <= 0) {
            return ManifestParseResult.Err("version_code 必須是正整數，收到 $versionCode")
        }

        val versionName = root.str("version_name")?.takeIf { it.isNotBlank() }
            ?: return ManifestParseResult.Err("版本資訊缺少 version_name")

        val size = root.long("size")
            ?: return ManifestParseResult.Err("版本資訊缺少 size")
        if (size <= 0 || size > MAX_APK_BYTES) {
            return ManifestParseResult.Err("size=$size 不合理（上限 $MAX_APK_BYTES）")
        }

        val sha256 = root.str("sha256")?.trim()?.lowercase()
            ?: return ManifestParseResult.Err("版本資訊缺少 sha256")
        if (!HEX64.matches(sha256)) {
            return ManifestParseResult.Err("sha256 不是 64 字元的十六進位字串")
        }

        val rawUrl = root.str("url")?.takeIf { it.isNotBlank() }
            ?: root.str("file")?.takeIf { it.isNotBlank() }
            ?: return ManifestParseResult.Err("版本資訊沒有 url 也沒有 file，不知道要下載什麼")
        // 相對路徑以 version.json 自己的位置為基底 —— 整個目錄搬到別的主機時
        // 不必改內容，本機測試也靠這一條。
        val url = NetworkGate.resolveUrl(manifestUrl, null, rawUrl)
        if (!url.startsWith("http://") && !url.startsWith("https://")) {
            return ManifestParseResult.Err("下載網址只接受 http/https：$url")
        }

        return ManifestParseResult.Ok(
            VersionManifest(
                versionCode = versionCode,
                versionName = versionName,
                commit = root.str("commit")?.takeIf { it.isNotBlank() },
                fileName = root.str("file")?.takeIf { it.isNotBlank() },
                size = size,
                sha256 = sha256,
                url = url,
                notes = root.str("notes")?.trim().orEmpty(),
            )
        )
    }
}

/** 版本比較的結果。刻意有 [DOWNGRADE] 這一項，見 [UpdateCheck.verdict]。 */
enum class UpdateVerdict {
    /** 遠端比較新，可以更新。 */
    UPDATE_AVAILABLE,

    /** 一樣新。 */
    UP_TO_DATE,

    /**
     * 遠端比本機**舊**。
     *
     * 不把它併進 [UP_TO_DATE]：這代表發布端出了事（回退了、或 version.json
     * 指到舊檔），而 Android 本來就不允許降級安裝，硬給使用者一顆「更新」
     * 按鈕只會讓他按下去失敗。要能區分才報得出有用的訊息。
     */
    DOWNGRADE,
}

object UpdateCheck {

    fun verdict(installedVersionCode: Long, remote: VersionManifest): UpdateVerdict = when {
        remote.versionCode > installedVersionCode -> UpdateVerdict.UPDATE_AVAILABLE
        remote.versionCode == installedVersionCode -> UpdateVerdict.UP_TO_DATE
        else -> UpdateVerdict.DOWNGRADE
    }

    /**
     * 下載完的檔案摘要是否符合宣告值。
     *
     * 大小寫不敏感、前後空白不算數：兩端各自產生這個字串（伺服器用
     * `sha256sum`，行動端用 `MessageDigest`），格式上的細節不該變成
     * 「明明是同一個檔案卻說不符」。內容不同才算不符。
     */
    fun sha256Matches(expected: String, actual: String): Boolean =
        expected.trim().equals(actual.trim(), ignoreCase = true)
}
