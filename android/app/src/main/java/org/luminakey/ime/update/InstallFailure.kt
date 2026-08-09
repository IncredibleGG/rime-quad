package org.luminakey.ime.update

import android.content.pm.PackageInstaller

/**
 * 把「系統拒絕安裝」翻成**一種原因**，而每一種原因都有它自己的下一步。
 *
 * ══════════════════════════════════════════════════════════════════════
 *  ⚠ 「APK 檔案無效或已損毀」是這一輪要消滅的那句話
 * ══════════════════════════════════════════════════════════════════════
 *
 * 使用者 2026-08-09 回報的畫面：
 *
 *     APK 檔案無效或已損毀。
 *     （系統訊息：INSTALL_FAILED_INVALID_APK: … specified package
 *      org.rimequad.ime inconsistent with org.luminakey.ime）  舊名
 *
 * 檔案完全正常 —— 它剛剛才通過 sha256 比對。真正的原因是產品改名換了
 * applicationId，而系統不允許覆蓋安裝。那句話把使用者指向**下載**，
 * 他會重試三次、每次都一樣，然後放棄。
 *
 * ── 因此這裡有一條硬規則 ────────────────────────────────────────────────
 * **這個列舉裡沒有「檔案損毀」。** 走到安裝這一步的檔案一定已經通過
 * [UpdateCheck.sha256Matches]（見 [UpdateController.downloadAndVerify]），
 * 也就是說我們**有證據**它與發布端宣告的位元組完全相同。在有那個證據的
 * 情況下說「檔案可能壞了」不是保守，是**說謊**，而且是往錯的方向說。
 * 摘要不符的那條路根本到不了這裡：下載階段就整包丟棄了。
 *
 * 真的要說「損毀」，只有在**驗過雜湊而且不符**的時候 —— 那句話住在
 * [UpdateController.downloadAndVerify]，不在這裡。
 *
 * ── 判定的順序：先看系統訊息，再看 status ───────────────────────────────
 * `PackageInstaller` 的 status 只有八種，粗到不足以分辨「簽章不符」與
 * 「套件名不同」（前者 CONFLICT、後者 INVALID，但兩者都可能以 INVALID 出現，
 * 取決於 Android 版本）。訊息字串裡的 `INSTALL_FAILED_*` 才是舊有的、
 * 分得夠細的那一組碼。
 *
 * ⚠ **但訊息字串沒有人承諾過。** 所以：訊息認得出來就用它（比較精確），
 * 認不出來一律退回 status（比較粗但一定在）。**兩者都認不出來時**回
 * [Kind.REJECTED_UNEXPLAINED] —— 那一句刻意承認「我們不知道」，
 * 而不是隨便挑一個聽起來合理的原因，因為挑錯的代價就是這一輪在修的東西。
 */
enum class InstallFailureKind {

    /** 使用者自己在系統的確認畫面按了取消。不是錯誤。 */
    CANCELLED,

    /**
     * 線上那一版的 applicationId 與本機不同 —— 系統把它當成另一個 app。
     * 唯一的路是：先匯出資料 → 解除安裝 → 安裝新版 → 匯入資料。
     */
    PACKAGE_ID_CHANGED,

    /** 不是同一把金鑰簽的。**不可以**叫使用者解除安裝硬裝（那會清掉他的詞典）。 */
    SIGNATURE_MISMATCH,

    /** 本機已經比這一份新。Android 不允許降級，這是發布端的錯。 */
    DOWNGRADE,

    /** 空間不足。使用者做得到的事：清出空間再按一次。 */
    NOT_ENOUGH_SPACE,

    /** 處理器架構或系統版本不合。使用者在這台機器上做什麼都不會改變結果。 */
    DEVICE_INCOMPATIBLE,

    /** 被裝置管理原則或掃描器（Play Protect）擋下。 */
    BLOCKED,

    /**
     * 系統拒絕了，而它給的理由我們認不出來。
     *
     * 這一項存在的意義就是**不要猜**。訊息會如實說「我們不知道為什麼」、
     * 說明檔案本身已經驗過、並請使用者把系統原文回報。
     */
    REJECTED_UNEXPLAINED,
}

object InstallFailure {

    /**
     * @param status `PackageInstaller.EXTRA_STATUS`
     * @param raw    `PackageInstaller.EXTRA_STATUS_MESSAGE`（可能是 null）
     */
    fun classify(status: Int, raw: String?): InstallFailureKind {
        val msg = raw.orEmpty()

        // ── 1. 系統訊息裡的舊碼（最精確）──────────────────────────────
        //
        // ⚠ 順序有意義：套件名不同要排在「INVALID_APK」之前判斷，因為
        //    它正是以 INSTALL_FAILED_INVALID_APK 的形式出現的。
        if (PackageIdentity.inconsistentPackages(msg) != null) {
            return InstallFailureKind.PACKAGE_ID_CHANGED
        }
        when {
            msg.containsAny(
                "INSTALL_FAILED_UPDATE_INCOMPATIBLE",
                "INSTALL_FAILED_SHARED_USER_INCOMPATIBLE",
                "INSTALL_PARSE_FAILED_INCONSISTENT_CERTIFICATES",
                "INSTALL_PARSE_FAILED_NO_CERTIFICATES",
                "INSTALL_FAILED_DUPLICATE_PERMISSION",
                "signatures do not match",
            ) -> return InstallFailureKind.SIGNATURE_MISMATCH

            msg.containsAny("INSTALL_FAILED_VERSION_DOWNGRADE") ->
                return InstallFailureKind.DOWNGRADE

            msg.containsAny("INSTALL_FAILED_INSUFFICIENT_STORAGE") ->
                return InstallFailureKind.NOT_ENOUGH_SPACE

            msg.containsAny(
                "INSTALL_FAILED_NO_MATCHING_ABIS",
                "INSTALL_FAILED_CPU_ABI_INCOMPATIBLE",
                "INSTALL_FAILED_OLDER_SDK",
                "INSTALL_FAILED_MISSING_SHARED_LIBRARY",
            ) -> return InstallFailureKind.DEVICE_INCOMPATIBLE

            msg.containsAny(
                "INSTALL_FAILED_VERIFICATION_FAILURE",
                "INSTALL_FAILED_VERIFICATION_TIMEOUT",
                "INSTALL_FAILED_USER_RESTRICTED",
                "INSTALL_FAILED_ADMIN_POLICY",
            ) -> return InstallFailureKind.BLOCKED

            msg.containsAny("INSTALL_FAILED_ABORTED") ->
                return InstallFailureKind.CANCELLED
        }

        // ── 2. 退回 status（粗，但一定在）─────────────────────────────
        return when (status) {
            PackageInstaller.STATUS_FAILURE_ABORTED -> InstallFailureKind.CANCELLED
            PackageInstaller.STATUS_FAILURE_BLOCKED -> InstallFailureKind.BLOCKED
            PackageInstaller.STATUS_FAILURE_STORAGE -> InstallFailureKind.NOT_ENOUGH_SPACE
            PackageInstaller.STATUS_FAILURE_INCOMPATIBLE -> InstallFailureKind.DEVICE_INCOMPATIBLE
            // CONFLICT 在沒有更精確的訊息時，最常見的成因就是簽章不符。
            PackageInstaller.STATUS_FAILURE_CONFLICT -> InstallFailureKind.SIGNATURE_MISMATCH
            // ⚠ INVALID **不**翻成「檔案損毀」。見檔頭。
            else -> InstallFailureKind.REJECTED_UNEXPLAINED
        }
    }

    private fun String.containsAny(vararg needles: String): Boolean =
        needles.any { contains(it, ignoreCase = true) }
}
