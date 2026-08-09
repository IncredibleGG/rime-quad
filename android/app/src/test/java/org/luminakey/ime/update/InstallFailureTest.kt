package org.luminakey.ime.update

import android.content.pm.PackageInstaller
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 安裝失敗的分類。
 *
 * ══════════════════════════════════════════════════════════════════════
 *  ⚠ 這一支存在的理由是一句**指向錯方向**的訊息
 * ══════════════════════════════════════════════════════════════════════
 *
 * 使用者 2026-08-09 收到：
 *
 *     APK 檔案無效或已損毀。
 *     （系統訊息：INSTALL_FAILED_INVALID_APK: … specified package
 *      org.rimequad.ime inconsistent with org.luminakey.ime）
 *
 * 檔案完全正常。真正的原因是產品改名換了 applicationId。那句話會讓他
 * 重下載三次、每次都一樣，然後放棄 —— 而他要做的其實是匯出、解除安裝、
 * 重裝、匯入。
 *
 * ⚠ `PackageInstaller` 的 status 常數是編譯期常數（`static final int`），
 *   kotlinc 會直接內聯成整數，所以這一支不需要 Android 執行環境。
 */
class InstallFailureTest {

    /** 使用者實際收到的那一則，一字不改。 */
    private val realWorldRaw =
        "INSTALL_FAILED_INVALID_APK: android.content.pm.parsing.ApkLite@fff160d " +
            "specified package org.rimequad.ime inconsistent with org.luminakey.ime"

    /* ═════════════ 1. 使用者真的踩到的那一格 ═════════════ */

    @Test
    fun `使用者收到的那一則被分成套件名不同`() {
        assertEquals(
            InstallFailureKind.PACKAGE_ID_CHANGED,
            InstallFailure.classify(PackageInstaller.STATUS_FAILURE_INVALID, realWorldRaw),
        )
    }

    /**
     * 反向：同一則訊息**絕不可以**再被歸成「檔案壞了」。
     *
     * 這一條寫成獨立的斷言而不是靠上面那條的相等性，是因為它守的是不同的
     * 東西：上面守「分對了」，這裡守「就算日後多一個分類，也不准往那個
     * 方向滑回去」。整個列舉裡刻意沒有「檔案損毀」那一項，這條測試順便
     * 釘住那個決定。
     */
    @Test
    fun `列舉裡沒有任何一項叫做檔案損毀`() {
        val names = InstallFailureKind.values().map { it.name }
        assertTrue(
            "走到安裝這一步的檔案一定通過了 sha256 —— 有證據還說「可能壞了」" +
                "不是保守，是往錯的方向說。實際：$names",
            names.none { it.contains("CORRUPT") || it.contains("INVALID") },
        )
    }

    /* ═════════════ 2. 四種必須分得出來的原因 ═════════════ */

    @Test
    fun `簽章不符`() {
        assertEquals(
            InstallFailureKind.SIGNATURE_MISMATCH,
            InstallFailure.classify(
                PackageInstaller.STATUS_FAILURE_INVALID,
                "INSTALL_FAILED_UPDATE_INCOMPATIBLE: Package org.luminakey.ime signatures " +
                    "do not match previously installed version; ignoring!",
            ),
        )
        // 沒有更精確的訊息時，CONFLICT 本身就代表簽章不符。
        assertEquals(
            InstallFailureKind.SIGNATURE_MISMATCH,
            InstallFailure.classify(PackageInstaller.STATUS_FAILURE_CONFLICT, null),
        )
    }

    @Test
    fun `版本降級`() {
        assertEquals(
            InstallFailureKind.DOWNGRADE,
            InstallFailure.classify(
                PackageInstaller.STATUS_FAILURE_INVALID,
                "INSTALL_FAILED_VERSION_DOWNGRADE",
            ),
        )
    }

    @Test
    fun `空間不足`() {
        assertEquals(
            InstallFailureKind.NOT_ENOUGH_SPACE,
            InstallFailure.classify(
                PackageInstaller.STATUS_FAILURE_INVALID,
                "INSTALL_FAILED_INSUFFICIENT_STORAGE",
            ),
        )
        assertEquals(
            InstallFailureKind.NOT_ENOUGH_SPACE,
            InstallFailure.classify(PackageInstaller.STATUS_FAILURE_STORAGE, null),
        )
    }

    @Test
    fun `裝置不合`() {
        for (code in listOf(
            "INSTALL_FAILED_NO_MATCHING_ABIS",
            "INSTALL_FAILED_CPU_ABI_INCOMPATIBLE",
            "INSTALL_FAILED_OLDER_SDK",
        )) {
            assertEquals(
                code,
                InstallFailureKind.DEVICE_INCOMPATIBLE,
                InstallFailure.classify(PackageInstaller.STATUS_FAILURE_INVALID, code),
            )
        }
        assertEquals(
            InstallFailureKind.DEVICE_INCOMPATIBLE,
            InstallFailure.classify(PackageInstaller.STATUS_FAILURE_INCOMPATIBLE, null),
        )
    }

    @Test
    fun `被擋下與被取消是兩件事`() {
        assertEquals(
            InstallFailureKind.BLOCKED,
            InstallFailure.classify(
                PackageInstaller.STATUS_FAILURE_BLOCKED,
                "INSTALL_FAILED_VERIFICATION_FAILURE",
            ),
        )
        assertEquals(
            InstallFailureKind.CANCELLED,
            InstallFailure.classify(PackageInstaller.STATUS_FAILURE_ABORTED, null),
        )
    }

    /* ═════════════ 3. 認不出來的時候 ═════════════ */

    /**
     * 認不出來就說「我們不知道」，不要挑一個聽起來合理的原因。
     *
     * 挑錯的代價就是這一輪在修的東西：一句自信、具體、而且錯的訊息，
     * 會讓看到它的人有信心地往錯的方向查。
     */
    @Test
    fun `認不出來時是 REJECTED_UNEXPLAINED 而不是隨便挑一個`() {
        assertEquals(
            InstallFailureKind.REJECTED_UNEXPLAINED,
            InstallFailure.classify(PackageInstaller.STATUS_FAILURE_INVALID, null),
        )
        assertEquals(
            InstallFailureKind.REJECTED_UNEXPLAINED,
            InstallFailure.classify(PackageInstaller.STATUS_FAILURE, "something brand new"),
        )
        assertEquals(
            InstallFailureKind.REJECTED_UNEXPLAINED,
            InstallFailure.classify(-99, null),
        )
    }

    /**
     * 訊息比 status 精確：同一個 status 配不同訊息要得到不同答案。
     * 這條反過來守著「別把訊息剖析拿掉改成只看 status」——
     * 那樣做的話，使用者踩到的那一格會退回 REJECTED_UNEXPLAINED，
     * 而搬家的步驟就再也不會出現在畫面上。
     */
    @Test
    fun `同一個 status 會因為訊息不同而分到不同格`() {
        val status = PackageInstaller.STATUS_FAILURE_INVALID
        val a = InstallFailure.classify(status, realWorldRaw)
        val b = InstallFailure.classify(status, "INSTALL_FAILED_VERSION_DOWNGRADE")
        val c = InstallFailure.classify(status, null)
        assertNotEquals(a, b)
        assertNotEquals(a, c)
        assertNotEquals(b, c)
    }
}
