package org.luminakey.ime.update

/**
 * 「線上那一版還是不是同一個 app」。
 *
 * ══════════════════════════════════════════════════════════════════════
 *  ⚠ 換 applicationId = 換一個 app。系統**不允許**覆蓋安裝
 * ══════════════════════════════════════════════════════════════════════
 *
 * 2026-08-09 產品改名（`org.rimequad.ime` → `org.luminakey.ime`，見
 * `docs/decisions/product-name.md` 與 `scripts/lib/product.env` 的
 * `ANDROID_APP_ID_PREVIOUS`）。對 Android 而言那是兩個毫無關係的 app：
 *
 *   · 裝不上去（`INSTALL_FAILED_INVALID_APK … inconsistent with …`）；
 *   · 就算使用者自己去別的地方裝，**詞典與設定一個都不會跟過去** ——
 *     舊 app 的私有資料目錄只有舊 app 讀得到，解除安裝時整個被系統刪掉。
 *
 * 第二點才是真正會讓使用者失去東西的地方，而它**沒有任何系統訊息會提到**。
 *
 * ── 這個檔案為什麼是純函式 ──────────────────────────────────────────────
 * 判定「一不一樣」不需要 Android，也不該需要。它決定的是「要不要下載 28MB
 * 然後讓系統拒絕」，所以要能被 JVM 單元測試直接打（[PackageIdentityTest]）。
 * 需要 `PackageManager` 的那一半（讀 APK 檔自己的套件名）住在
 * [UpdateInstaller.packageNameOf]。
 */
object PackageIdentity {

    /**
     * 線上那一版與本機的關係。
     *
     * [UNKNOWN] 是**第一級公民**，不是「大概一樣吧」的同義詞：舊的
     * version.json 沒有 `package` 欄位，而使用者手上跑的可能是任何一版。
     * 把「不知道」當成「一樣」會讓這條線再次退化成「下載完才發現」，
     * 所以呼叫端必須自己決定不知道的時候要怎麼辦（我們的答案是：照常
     * 下載，但**安裝前**改讀 APK 檔本身，見 [UpdateInstaller.packageNameOf]）。
     */
    enum class Verdict { SAME, UNKNOWN, CHANGED }

    fun compare(installed: String, remote: String?): Verdict = when {
        remote.isNullOrBlank() -> Verdict.UNKNOWN
        remote == installed -> Verdict.SAME
        else -> Verdict.CHANGED
    }

    fun compare(installed: String, remote: VersionManifest): Verdict =
        compare(installed, remote.packageId)

    /**
     * 線上那一版有沒有**明說**它取代的就是我們。
     *
     * 分得出兩件事，而兩件事該說的話完全不同：
     *
     *   · 有宣告 → 這是我們自己改名。告訴使用者怎麼把資料搬過去。
     *   · 沒宣告 → 這份 version.json 可能根本不是給這個 app 的（設定指錯了
     *     網址、或某個目錄被搬動過）。那時候要他去裝一個「不知道是什麼」的
     *     APK 是不負責任的，訊息必須保守。
     */
    fun declaresReplacing(installed: String, remote: VersionManifest): Boolean =
        remote.replacesPackages.any { it == installed }

    /**
     * 看起來像不像一個 Android 套件名。
     *
     * 刻意寬鬆（不驗保留字、不驗每一段的首字元不得為數字）：這裡的用途是
     * **把垃圾擋在比對之外**，不是當一個 dex 驗證器。真正的權威是系統，
     * 而系統會在安裝時說話。
     *
     * 但空白、控制字元與「沒有點」一定要擋掉 —— 那種值進到比對裡，會讓
     * 「一不一樣」得出一個看起來確定、實際上沒有根據的答案。
     */
    fun looksLikePackageName(s: String): Boolean =
        s.isNotBlank() && s.length <= 255 && PACKAGE_NAME.matches(s)

    private val PACKAGE_NAME = Regex("""[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z0-9_]+)+""")

    /**
     * 從系統的安裝失敗訊息裡把兩個套件名撈出來。
     *
     * 實際收到的長相（使用者 2026-08-09 回報的那一則）：
     *
     *     INSTALL_FAILED_INVALID_APK: android.content.pm.parsing.ApkLite@fff160d
     *     specified package org.rimequad.ime inconsistent with org.luminakey.ime
     *
     * ⚠ **這是在剖析一個沒有人承諾過的字串。** AOSP 換一個字我們就撈不到 ——
     * 所以撈不到**不可以**讓分類失敗：[InstallFailure] 只拿它當「更精確的
     * 說法」，判定本身另有依據。撈得到就多說兩個名字給使用者看，撈不到就
     * 少說那一句。
     *
     * @return (安裝包裡宣告的, 系統期待的)；認不出來時 null。
     */
    fun inconsistentPackages(raw: String?): Pair<String, String>? {
        val m = INCONSISTENT.find(raw ?: return null) ?: return null
        return m.groupValues[1] to m.groupValues[2]
    }

    private val INCONSISTENT = Regex(
        """specified package\s+([A-Za-z0-9_.]+)\s+inconsistent with\s+([A-Za-z0-9_.]+)"""
    )
}
