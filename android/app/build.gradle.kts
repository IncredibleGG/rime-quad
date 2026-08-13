import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import java.security.MessageDigest
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

// 應用身分一律從 gradle.properties 取，見該檔註解。
val rimeApplicationId: String = providers.gradleProperty("rime.applicationId").get()
val rimeNamespace: String = providers.gradleProperty("rime.namespace").get()
val rimeJniClass: String = providers.gradleProperty("rime.jniClass").get()

// ─────────────────────────────────────────────────────────────────────────────
// 隨附執行期資料，共兩類：
//
//  1. librime 的執行期資料（schema、詞庫、opencc、essay 語言模型）。
//     由 scripts/collect_data.sh 產生，體積 13MB，且**刻意不進版控**
//     （見專案根目錄 .gitignore 的 /core/data/）。
//
//  2. 四端共用的宣告式配置：core/layouts（鍵盤佈局）與 core/themes（主題）。
//     這一類**有**進版控，體積很小，但同樣不複製進 src/main/assets ——
//     那會產生第二份會腐爛的副本，而且先前已被否決。唯一的真相是 core/。
//
// 兩類都走同一條 Sync 任務進 build/ 底下的 generated assets 目錄，
// 產生物留在產生物該在的地方。
//
// ⚠ **沒跑過 collect_data.sh 的機器現在建置不起來**（從前只是警告）。
//   擋下來的是 `generateEnginePageSize`：設定頁那個「一次顯示幾個候選」的
//   上限要從 `core/data/shared/default.yaml` 的 `menu/page_size` 產生，缺那份
//   資料就無從得知。給一個猜出來的預設值，正是上一次讓設定頁對使用者說謊的
//   做法（理由寫在那支任務的檔頭）。
//   而且那樣的 APK 本來就一個方案都沒有、一個字都打不出來 —— 從前那條
//   「警告一聲照樣出一份壞掉的 APK」的路，本身就是本專案再三提防的形狀。
//   底下 syncRimeData 的 doFirst 仍然留著警告：它先跑，訊息比較具體。
// ─────────────────────────────────────────────────────────────────────────────
val rimeRepoRoot = layout.projectDirectory.dir("../..")
val rimeSharedData = rimeRepoRoot.dir("core/data/shared")
val rimeUserData = rimeRepoRoot.dir("core/data/user")
val rimeLayouts = rimeRepoRoot.dir("core/layouts")
val rimeThemes = rimeRepoRoot.dir("core/themes")
val rimeSchemaLanguages = rimeRepoRoot.file("core/schema-languages.json")
// 「按數字鍵真的選得到第 N 個嗎」的實測表(§8.6.1.1)。與上面那一份一樣是
// 宣告式資料,由 scripts/verify_selection_digit.sh 在真機上量出來。
val rimeSelectionDigits = rimeRepoRoot.file("core/selection-digit.tsv")

// 主題／佈局格式規範。不是建置的輸入，是**測試**的輸入：
// DiagnosticCodeSpecTest 直接讀 §6.5.1 的碼表（見下方 tasks.withType<Test>）。
val rimeThemeFormatSpec = rimeRepoRoot.file("docs/theme-format.md")
val rimeGeneratedAssets = layout.buildDirectory.dir("generated/rimeAssets")

val syncRimeData = tasks.register<Sync>("syncRimeData") {
    description = "把 core/data、core/layouts、core/themes 同步進 generated assets"
    into(rimeGeneratedAssets)
    from(rimeSharedData) { into("rime/shared") }
    // ── 使用者初始配置：行動端那一份要覆蓋掉四端共用的那一份 ─────────────
    //
    // `collect_data.sh` 產出兩個檔（見那支的第 7 節）：
    //   default.custom.yaml         四端共用
    //   default.custom.mobile.yaml  行動端 = 共用的那一份 ＋ 觸控鍵盤專屬的幾條
    //
    // librime 只認得 `default.custom.yaml` 這一個檔名，所以行動端那一份要
    // **改名蓋上去**。兩個 from 加 DuplicatesStrategy.INCLUDE：同一個目的路徑
    // 出現兩次時後面那個勝出。
    //
    // ⚠ 順序不能反，而且第一個 from 刻意**不排除** default.custom.yaml：
    //   舊的 core/data（還沒跑過新版 collect_data.sh，例如別條線 symlink 過來的
    //   那一份）裡沒有 .mobile.yaml，那時第二個 from 貢獻零個檔案，
    //   APK 裡留下的是共用那一份 —— schema_list 仍然完整，只是少了行動端的
    //   那幾條微調。**少一項設定**與**整個方案清單消失**是兩件事，
    //   而後者正是 coordination.md §1 記著的那場事故。
    from(rimeUserData) {
        into("rime/user")
        exclude("*.mobile.yaml")
    }
    from(rimeUserData) {
        into("rime/user")
        include("default.custom.mobile.yaml")
        rename { "default.custom.yaml" }
    }
    duplicatesStrategy = DuplicatesStrategy.INCLUDE
    // 佈局與主題：assets 內的路徑刻意與 docs/theme-format.md §2.3 的
    // 「隨附目錄」同名，四端的搜尋路徑因此長得一樣。
    from(rimeLayouts) { into("rime/layouts") }
    from(rimeThemes) { into("rime/themes") }
    // 方案 → BCP 47 語言標記的對照表（scripts/schema_store/languages.py 產生）。
    // 選單分組讀的是這一份加上索引裡的 language 欄位；兩者都沒有時才退回啟發式。
    from(rimeSchemaLanguages) { into("rime") }
    from(rimeSelectionDigits) { into("rime") }
    doFirst {
        if (!rimeSharedData.asFile.isDirectory) {
            logger.warn(
                "[rime] 找不到 ${rimeSharedData.asFile}，APK 將不含任何 schema。" +
                    " 先跑 scripts/collect_data.sh。"
            )
        }
        if (!rimeLayouts.asFile.isDirectory || !rimeThemes.asFile.isDirectory) {
            logger.warn("[rime] 找不到 core/layouts 或 core/themes，鍵盤將畫不出來。")
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// shared 資料的內容摘要
//
// 執行期靠它判斷「裝置上那份 shared 還是不是這一版 APK 的那一份」
// （見 RimeRuntime.ASSET_SHARED_DIGEST）。從前那裡是一個手寫的
// `ASSET_REVISION = 3`，於是**改**方案內容而沒改那個常數時，升級上來的裝置
// 一個檔案都不重解 —— 真機回報「選了 ni 卻上屏 ni好」就是這麼來的，
// 而全新安裝的機器（CI、模擬器）永遠重現不出來。
//
// ⚠ 刻意**不做** up-to-date 檢查（`upToDateWhen { false }`）：這支任務存在的
// 唯一理由就是「摘要不可以過期」，讓 Gradle 幫它跳過等於把同一個坑再挖一次。
// 代價是每次建置多雜湊一次 13MB，量到約 40ms。
// ─────────────────────────────────────────────────────────────────────────────
val rimeAssetStamp = layout.buildDirectory.dir("generated/rimeAssetStamp")

val stampRimeData = tasks.register("stampRimeData") {
    description = "把 core/data/shared 的內容摘要寫進 assets"
    val src = rimeSharedData
    val outDir = rimeAssetStamp
    outputs.dir(outDir)
    outputs.upToDateWhen { false }
    doLast {
        val root = src.asFile
        val md = MessageDigest.getInstance("SHA-256")
        if (root.isDirectory) {
            root.walkTopDown()
                .filter { it.isFile }
                .map { it.toRelativeString(root).replace(File.separatorChar, '/') to it }
                // 排序是必要的:目錄走訪順序不保證，沒排的話同一份資料在兩台
                // 機器上會算出兩個摘要，於是每次換機器建置都強迫使用者重解一次。
                .sortedBy { it.first }
                .forEach { (rel, f) ->
                    md.update(rel.toByteArray(Charsets.UTF_8))
                    md.update(0)
                    f.inputStream().use { ins ->
                        val buf = ByteArray(1 shl 16)
                        while (true) {
                            val n = ins.read(buf)
                            if (n <= 0) break
                            md.update(buf, 0, n)
                        }
                    }
                }
        } else {
            logger.warn("[rime] 找不到 ${root}，shared 摘要會是空目錄的摘要。")
        }
        val hex = md.digest().joinToString("") { "%02x".format(it) }
        val out = outDir.get().file("rime/shared.digest").asFile
        out.parentFile.mkdirs()
        out.writeText(hex)
        logger.lifecycle("[rime] shared 摘要 $hex")
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 引擎一頁給幾個候選 —— 從隨附資料產生，不准手寫
//
// `PrefLevels.ENGINE_PAGE_SIZE` 從前是一個手寫的 `= 5`。它是設定頁那個
// 「一次顯示幾個候選」的上限：引擎一頁只給這麼多，前端的 `max_visible`
// 只能從那一頁裡挑得更少，挑不出更多。
//
// ── 為什麼不可以是手寫的常數 ────────────────────────────────────────────
// 那個 5 抄自 `core/data/shared/default.yaml` 的 `menu/page_size`，而抄本
// **不知道正本改了**。實際發生過：一條線量到引擎給 5 個，於是把檔位砍成
// 3/4/5 並寫下常數 5；另一條線同時把 `menu/page_size` 改成 9。兩邊各自
// 都對，合起來使用者拿到的是「畫面畫 9 個、設定列顯示 5 個，碰一下就永久
// 鎖在 5」——而守著檔位的那條測試不會紅，因為 3/4/5 確實都 ≤ 9。
//
// ── 為什麼是建置期產生，不是執行期問引擎 ────────────────────────────────
// 「執行期問引擎」看起來更準（第三方方案可以在自己的 schema 裡覆寫
// `menu/page_size`），但那條路解不掉這個問題：
//
//   · **這個數字要在兩個行程裡用。** 設定頁在 App 行程（SettingsPages），
//     鍵盤上的就地編輯器在輸入法行程。App 那邊沒有、也不該有一個活著的
//     rime session —— 為了問一個整數去部署一次引擎，是拿數秒的啟動換一個
//     開機就知道的常數。
//   · **檔位清單會在使用者腳下變形。** 檔位的標籤是 strings.xml 裡的靜態
//     字串陣列（三個語系各一份，StringCatalogTest 釘住長度）。跟著當前方案
//     浮動的話，換一次方案設定頁就少一格或多一格。
//   · **測不到。** 這一輪的教訓正是「守門是綠的而產品是壞的」。純建置期的
//     常數才守得住 —— 見 EnginePageSizeTest。
//
// 第三方方案宣告了更小的 `page_size` 時，`take(cap)` 自然拿到比較少的候選，
// 不會錯位、不會當掉；那是「選了 9 但這個方案只給得出 5」，與從前那個
// 「選了 9 而**任何**方案都只給 5」不是同一件事。已記在
// docs/coordination.md §5。
//
// ⚠ **讀不到就爆，不給預設值。** 「拿一個看起來合理的數字繼續」正是這一輪
//   在修的那個缺陷本身。`scripts/collect_data.sh` 覆寫這個鍵時用的也是同一
//   條規矩（改不到就 die）。
// ─────────────────────────────────────────────────────────────────────────────
val rimeSharedDefaultYaml = rimeRepoRoot.file("core/data/shared/default.yaml")
val rimeGeneratedSrc = layout.buildDirectory.dir("generated/rimeEnginePageSize")

/**
 * 從 `default.yaml` 讀出**頂層** `menu:` 底下的 `page_size`。
 *
 * 刻意不是 `grep page_size`：同一份資料裡 `stroke.schema.yaml` 也有一個
 * `page_size`，而方案自己的那一個與 default 的預設不是同一件事。
 */
fun parseMenuPageSize(f: File): Int {
    var inMenu = false
    for (line in f.readLines()) {
        val trimmed = line.trim()
        if (trimmed.isEmpty() || trimmed.startsWith("#")) continue
        if (!line[0].isWhitespace()) {
            inMenu = line.trimEnd().removeSuffix(":") == "menu"
            continue
        }
        if (!inMenu) continue
        if (trimmed.startsWith("page_size:")) {
            val v = trimmed.removePrefix("page_size:").trim().toIntOrNull()
            if (v != null && v > 0) return v
        }
    }
    throw GradleException(
        "在 ${f.absolutePath} 裡找不到頂層 menu/page_size。\n" +
            "上游可能改了鍵名。確認之後同時更新 scripts/collect_data.sh 的覆寫" +
            "與這裡的解析 —— 不要改成給一個預設值,那正是這一段存在的理由。"
    )
}

val generateEnginePageSize = tasks.register("generateEnginePageSize") {
    description = "把 core/data/shared/default.yaml 的 menu/page_size 產生成 Kotlin 常數"
    val src = rimeSharedDefaultYaml
    val outDir = rimeGeneratedSrc
    // ⚠ `inputs.files(...)` 而不是 `inputs.file(...)`。後者會在**任務開始前**
    //   做存在性驗證,於是檔案不在的時候使用者拿到的是 Gradle 那句
    //   「Property '$1' specifies file ... which doesn't exist」——
    //   一句不告訴他該跑什麼的訊息。用 FileCollection 就允許不存在,
    //   由底下的 doLast 給出那句「先跑 scripts/collect_data.sh」。
    inputs.files(src).withPathSensitivity(PathSensitivity.RELATIVE)
        .withPropertyName("sharedDefaultYaml")
    outputs.dir(outDir)
    doLast {
        val f = src.asFile
        if (!f.isFile) {
            throw GradleException(
                "找不到 ${f.absolutePath}。\n" +
                    "先跑 scripts/collect_data.sh —— 沒有那份資料,APK 裡一個方案都" +
                    "沒有,而且「引擎一頁給幾個」無從得知。\n" +
                    "這裡刻意不退回一個預設值:一個猜出來的常數正是上一次讓" +
                    "設定頁對使用者說謊的東西。"
            )
        }
        val n = parseMenuPageSize(f)
        val out = outDir.get().file("org/luminakey/ime/prefs/EnginePageSize.kt").asFile
        out.parentFile.mkdirs()
        out.writeText(
            """
            // 產生的檔案 —— 不要手改,改了下一次建置就會被蓋掉。
            // 來源:core/data/shared/default.yaml 的 menu/page_size
            // 產生者:android/app/build.gradle.kts 的 generateEnginePageSize
            package org.luminakey.ime.prefs

            /** 引擎一頁給幾個候選。見 [PrefLevels.ENGINE_PAGE_SIZE]。 */
            internal const val ENGINE_PAGE_SIZE_FROM_DATA: Int = $n
            """.trimIndent() + "\n"
        )
        logger.lifecycle("[rime] 引擎一頁 $n 個候選(來自 core/data/shared/default.yaml)")
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 版本號
//
// versionCode 從前是寫死的 1，永遠不變。那讓兩件事同時壞掉：
//   · Android 自己的升級語意 —— 系統允許同號覆蓋安裝，但「哪一份比較新」
//     從此無從判斷，降級也擋不住。
//   · app 內的更新檢查 —— 沒有一個單調遞增的數字可以比。
//
// 推導方式：**HEAD commit 的 committer 時間（UTC）**格式化成 `yyMMddHH`。
//   例：2026-08-07 21:05 UTC → 26080721
//
// 為什麼是 commit 時間而不是建置時間：
//   · **可重現**。同一個 commit 在任何機器、任何時候建，都得到同一個號碼。
//     建置時間辦不到，而且會讓「同一份程式碼」產生兩個版本號。
//   · **單調**。commit 時間只會往前走（rebase／amend 也會把 committer date
//     更新成當下）。
//
// 已知限制：同一個小時內的兩個 commit 會拿到同一個號碼。手動側載發布的節奏
// 撞不到這件事；真的撞到時用 `-Prime.versionCode=<n>` 覆寫（測試升級流程時
// 也是靠這個造出一個「比較新」的 APK）。
//
// 沒有 git 的環境（例如從 tarball 解出來建）退回 1，並印警告。
// ─────────────────────────────────────────────────────────────────────────────
fun gitOutput(vararg args: String): String? = try {
    val proc = ProcessBuilder(listOf("git") + args)
        .directory(rimeRepoRoot.asFile)
        .redirectErrorStream(false)
        .apply { environment()["TZ"] = "UTC" }
        .start()
    val out = proc.inputStream.bufferedReader().readText().trim()
    proc.errorStream.close()
    if (proc.waitFor() == 0 && out.isNotEmpty()) out else null
} catch (e: Exception) {
    null
}

val rimeGitSha: String =
    providers.gradleProperty("rime.gitSha").orNull
        ?: gitOutput("rev-parse", "--short=7", "HEAD")
        ?: "nogit"

val rimeVersionCode: Int = run {
    val override = providers.gradleProperty("rime.versionCode").orNull?.toIntOrNull()
    val fromGit = gitOutput("log", "-1", "--format=%cd", "--date=format-local:%y%m%d%H")
        ?.toIntOrNull()
    override ?: fromGit ?: 1
}

/** 語意版本的人類可讀部分。改版時只改這一行。 */
val rimeVersionBase = "0.1.0-dev"

/** 例：`0.1.0-dev+26080721.3c8a8d9`。帶 sha 是為了讓使用者回報的版本對得回 commit。 */
val rimeVersionName = "$rimeVersionBase+$rimeVersionCode.$rimeGitSha"

// ─────────────────────────────────────────────────────────────────────────────
// 簽章
//
// ── 為什麼這件事是核心而不是雜務 ────────────────────────────────────────
// Android 強制：升級一個已安裝的 app，新 APK 必須由同一把金鑰簽。簽錯了
// 系統直接拒絕，使用者只能解除安裝重裝 —— 連帶失去個人詞典與所有設定。
// 也就是說，**擋住惡意「升級」的不是我們自己算的 sha256，是簽章**。
// sha256 只防傳輸損壞（它和 APK 來自同一台伺服器，伺服器被入侵時兩者一起被換）。
//
// ── 密碼從哪來 ──────────────────────────────────────────────────────────
// 從 repo **之外**的 `signing.properties` 讀。這個檔案不進版控，路徑可用
// `-Prime.signingDir=` 或環境變數 `RIME_SIGNING_DIR` 覆寫，預設 `~/rime-signing`。
// 找不到就退回 Android 預設的 debug 簽章：別人 clone 下來仍然編得起來，
// 只是編出來的 APK 升級不了正式版。這一點會在建置時印出來，不靜默。
// ─────────────────────────────────────────────────────────────────────────────
val rimeSigningDir: File = file(
    providers.gradleProperty("rime.signingDir").orNull
        ?: providers.environmentVariable("RIME_SIGNING_DIR").orNull
        ?: "${System.getProperty("user.home")}/rime-signing"
)

val rimeSigningProps: Properties? =
    File(rimeSigningDir, "signing.properties").takeIf { it.isFile }?.let { f ->
        Properties().apply { f.inputStream().use { load(it) } }
    }

/**
 * 輪替證明（SigningCertificateLineage）。由 `apksigner rotate` 從舊的 debug
 * 金鑰產生，內容是「舊金鑰授權新金鑰接手」。沒有它，換金鑰 = 所有使用者
 * 被迫重裝。
 */
val rimeLineageFile: File? = rimeSigningProps?.getProperty("lineageFile")
    ?.let { File(it) }?.takeIf { it.isFile }

android {
    namespace = rimeNamespace
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    testOptions {
        // LayoutHost 是純狀態機,唯一的 android 相依是 android.util.Log。
        // 預設的 "not mocked" 例外會讓它只能靠儀器測試守住,而「切出去回不來」
        // 這類缺陷正是最該由 JVM 測試擋下的。
        unitTests.isReturnDefaultValues = true
    }

    defaultConfig {
        applicationId = rimeApplicationId
        minSdk = 26
        targetSdk = 35
        versionCode = rimeVersionCode
        versionName = rimeVersionName

        // method.xml 的 android:settingsActivity 透過字串資源引用，
        // 免得改套件名時漏改 res/xml。
        resValue("string", "ime_settings_activity", "$rimeNamespace.MainActivity")

        // 方案市集的索引位置。docs/schema-store.md §5 說得很清楚：從 R2 搬到
        // GitHub Releases 時「索引格式不需要變動，只需換掉 base_url」——
        // 所以來源必須是可設定的，不能寫死在程式裡。
        //
        // 這裡是**預設值**；使用者可以在市集畫面改，改過的值存在
        // SharedPreferences（見 StoreSettings）。本機驗證時指向
        //   http://127.0.0.1:8099/index.json
        // 搭配 `adb reverse tcp:8099 tcp:8099` 與 android/testdata/store。
        buildConfigField(
            "String",
            "SCHEMA_INDEX_URL",
            "\"" + (providers.gradleProperty("rime.indexUrl").orNull
                ?: "https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev/rime/schemas/index.json") + "\"",
        )

        // 應用內更新用的版本資訊檔（scripts/publish_apk.sh 產生並上傳）。
        // 與索引同理：發布位置會變，所以是可設定的而不是寫死在程式裡。
        // 驗證升級流程時指向 rime/test/version.json，不去動使用者正在用的那一份。
        buildConfigField(
            "String",
            "VERSION_MANIFEST_URL",
            "\"" + (providers.gradleProperty("rime.versionManifestUrl").orNull
                ?: "https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev/rime/version.json") + "\"",
        )

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += listOf(
                    // 必須與 third_party/prebuilt/manifest.json 的 toolchain.stl 一致，
                    // 否則 librime.a 與本模組會對到不同的 libc++。
                    "-DANDROID_STL=c++_static",
                    "-DRIME_JNI_CLASS=$rimeJniClass",
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    signingConfigs {
        if (rimeSigningProps != null) {
            create("rime") {
                storeFile = file(rimeSigningProps.getProperty("storeFile"))
                storePassword = rimeSigningProps.getProperty("storePassword")
                keyAlias = rimeSigningProps.getProperty("keyAlias")
                keyPassword = rimeSigningProps.getProperty("keyPassword")
                // v1 給 API 26–27（minSdk 是 26，那些裝置看不懂 v3）。
                // v3 是輪替真正生效的區塊。v4 用不到（那是 adb incremental install
                // 的東西，會多產一個 .idsig 檔），關掉。
                enableV1Signing = true
                enableV2Signing = true
                enableV3Signing = true
                enableV4Signing = false
            }
        }
    }

    buildTypes {
        // debug 與 release 都用同一把正式金鑰。
        //
        // 為什麼 debug 也要：驗證腳本在模擬器上裝的是 debug 建置，而使用者手上
        // 是 release。兩邊若用不同的金鑰，「覆蓋安裝上得去嗎」在本機就永遠測不到
        // ——那正是這條線最貴的一種缺陷（升不上去 = 使用者只能重裝、失去詞典）。
        //
        // ── 2026-08-10：發給使用者的從 debug 換成 release ────────────────────
        // 在此之前 scripts/publish_apk.sh 預設發的是 app-debug.apk，而 debug 建置
        // 的 android:debuggable 預設是 true。後果不是「開發者方便」而是：
        // 任何拿得到 adb 的人都能 `run-as org.luminakey.ime` 把使用者的詞庫與
        // 輸入歷史整包讀走，也能對輸入法進程掛除錯器 —— 而輸入法看得到使用者
        // 打的每一個字。這與「離線為預設、經得起審計」的產品定位直接衝突。
        //
        // debug 建置**仍然是 debuggable**，那是刻意的：模擬器上的驗證腳本要靠
        // run-as 讀資料目錄、要靠 src/debug 的 harness 驅動匯出/匯入。
        // 兩者的分工由 scripts/release_check.sh 第 3c 關釘住：
        // 「release 那份不是 debuggable 且不含 harness，debug 那份兩者皆是」。
        // 後半句是正控 —— 少了它，偵測方法自己壞掉的那天這一關會安靜地全綠。
        val rimeSigning = signingConfigs.findByName("rime")
        debug {
            isJniDebuggable = true
            rimeSigning?.let { signingConfig = it }
        }
        release {
            // 這兩行是 AGP 的預設值，寫出來是因為它們現在是**產品承諾**而不是
            // 建置細節。留白的預設值改起來沒有阻力也不留痕跡；寫成一行，
            // 要改的人得先把上面那段註解讀完。
            isDebuggable = false
            isJniDebuggable = false
            // ⚠ 先不開 R8。開了要動 Compose 與 JNI（RegisterNatives 之外仍有
            //   以名稱查的 native callback），那是另一件要單獨驗的事。
            isMinifyEnabled = false
            rimeSigning?.let { signingConfig = it }
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    packaging {
        resources.excludes += setOf("/META-INF/{AL2.0,LGPL2.1}")
        jniLibs.useLegacyPackaging = false
    }

    sourceSets.getByName("main").assets.srcDir(rimeGeneratedAssets)
    // 摘要走**另一個** srcDir，不寫進 syncRimeData 的目的地:Sync 會刪掉
    // 目的地裡不屬於它的檔案，摘要放進去每次建置都會被刪掉再寫回來。
    sourceSets.getByName("main").assets.srcDir(rimeAssetStamp)
    // 引擎一頁幾個候選:產生出來的常數當成一份 main 原始碼。
    sourceSets.getByName("main").java.srcDir(rimeGeneratedSrc)
}

// assets 合併之前必須先同步完資料。
tasks.matching { it.name.startsWith("merge") && it.name.endsWith("Assets") }
    .configureEach { dependsOn(syncRimeData, stampRimeData) }
tasks.named("preBuild") { dependsOn(syncRimeData, stampRimeData, generateEnginePageSize) }
// Kotlin 編譯要等常數產生出來。掛 preBuild 不夠:AGP 的 compile 任務不保證
// 排在它後面,而漏掉的症狀是「第一次建置找不到 ENGINE_PAGE_SIZE_FROM_DATA,
// 第二次就好了」——那種偶發失敗最難查。
tasks.withType<org.jetbrains.kotlin.gradle.tasks.KotlinCompile>()
    .configureEach { dependsOn(generateEnginePageSize) }

// ─────────────────────────────────────────────────────────────────────────────
// 單元測試也吃 core/layouts 與 core/themes
//
// 那幾份 YAML 不是測試資源，是**四端真的會載入的檔案** —— RepoFixtures 刻意
// 直接讀 repo，不複製一份（複製品會腐爛）。但 Gradle 只認得模組裡的檔案，
// 於是「只改 YAML」的那一次，`:app:testDebugUnitTest` 會判 UP-TO-DATE：
//
//     > Task :app:testDebugUnitTest UP-TO-DATE
//     BUILD SUCCESSFUL
//
// 一整組守著佈局與主題的測試（LayoutEscapeTest、ThemeParserTest、MiniYamlTest、
// DeadKeyTest…）一個都沒跑，而畫面上是一片綠。實測過：把 qwerty 的一顆鍵指向
// 未實作的動詞，測試沒有變紅；補上 --rerun-tasks 才紅。
//
// 這正是本專案再三提防的「該紅的時候安靜地不跑」，而且它發生在建置工具層，
// 比測試自己寫錯更難發現。宣告成輸入之後，改 YAML 就會重跑。
// ─────────────────────────────────────────────────────────────────────────────
tasks.withType<Test>().configureEach {
    inputs.dir(rimeLayouts)
        .withPathSensitivity(PathSensitivity.RELATIVE)
        .withPropertyName("rimeLayouts")
    inputs.dir(rimeThemes)
        .withPathSensitivity(PathSensitivity.RELATIVE)
        .withPropertyName("rimeThemes")
    // res/ 同理,而且更隱蔽。StringCatalogTest 與 PanelStringsTest 直接讀
    // src/main/res/values*/strings.xml 的**原始檔**(讀 R 沒有用:aapt 早就
    // 把「這個語系缺這個 key」回落成預設值了)。但 testDebugUnitTest 的輸入
    // 只有編譯產物與 R.jar,而 R.jar 只在**新增或刪除 key** 時才變 ——
    // 改一句翻譯的文字,什麼都不會失效:
    //
    //     > Task :app:testDebugUnitTest UP-TO-DATE
    //
    // 也就是說,這個專案的在地化防線在「只改翻譯」的那一次從來沒跑過,
    // 而那正是它唯一該跑的時候。
    inputs.dir(layout.projectDirectory.dir("src/main/res"))
        .withPathSensitivity(PathSensitivity.RELATIVE)
        .withPropertyName("appResources")
    // docs/theme-format.md 同理,而且是跨端的:DiagnosticCodeSpecTest 直接讀
    // §6.5.1 的診斷碼表,拿它跟 DiagnosticCode 逐項比對(抄一份常數表會腐爛,
    // 而腐爛的方式正好是「規範改了、測試還是綠的」)。規範由 macOS 端維護、
    // 不在這個模組裡,不宣告成輸入的話它改了這邊會判 UP-TO-DATE ——
    // 那正是這個專案吃過兩次虧的「該紅的時候安靜地不跑」。
    inputs.file(rimeThemeFormatSpec)
        .withPathSensitivity(PathSensitivity.RELATIVE)
        .withPropertyName("rimeThemeFormatSpec")
    // core/data/shared/default.yaml 同理。EnginePageSizeTest 當場讀它的
    // menu/page_size,拿去跟 PrefLevels.ENGINE_PAGE_SIZE 比對。不宣告成輸入的
    // 話,「只改了 page_size」的那一次會判 UP-TO-DATE —— 而那正是這條測試
    // 唯一該跑的時候。這一整段的教訓就是這麼來的。
    inputs.file(rimeSharedDefaultYaml)
        .withPathSensitivity(PathSensitivity.RELATIVE)
        .withPropertyName("rimeSharedDefaultYaml")
}

// ─────────────────────────────────────────────────────────────────────────────
// 帶 lineage 重簽（金鑰輪替）
//
// AGP 的 `signingConfigs` 截至 8.x **沒有**暴露 lineage 參數，所以 Gradle 簽完
// 之後得再用 apksigner 重簽一次，把輪替證明塞進 v3 區塊。少了這一步，
// 換金鑰的 APK 在已裝舊版的機器上會被系統擋掉
// （INSTALL_FAILED_UPDATE_INCOMPATIBLE），使用者只能解除安裝重裝、失去詞典。
//
// 簽章者順序必須與 lineage 一致：**舊金鑰在前、新金鑰在後**。apksigner 用
// 最舊的那把簽 v1／v2（這兩個格式沒有輪替的概念，API 28 以下只認得原本那把），
// 新的那把簽 v3 —— 所以 release APK 裡同時存在兩把金鑰的簽章是正常的。
//
// `--rotation-min-sdk-version 28`：不指定的話 apksigner 會把輪替後的金鑰放進
// v3.1 區塊，而 v3.1 只有 Android 13(33)+ 認得，API 28–32 會退回舊金鑰。
// 指定一個 <= 32 的值改用不帶平台標記的 v3 區塊，讓輪替從 API 28 起就生效。
// ─────────────────────────────────────────────────────────────────────────────
val rimeApksigner: File =
    File(android.sdkDirectory, "build-tools/${android.buildToolsVersion}/apksigner")

when {
    rimeSigningProps == null -> logger.warn(
        "[rime] 找不到 $rimeSigningDir/signing.properties，本次用 Android 預設 debug 金鑰簽。\n" +
            "       這樣建出來的 APK **無法**升級正式發布的版本（簽章不符，系統會拒絕）。"
    )

    rimeLineageFile == null -> logger.warn(
        "[rime] 有簽章設定但找不到 lineage 檔（signing.properties 的 lineageFile）。\n" +
            "       APK 會用新金鑰簽但**不含輪替證明**，舊使用者升不上去。"
    )

    !rimeApksigner.canExecute() -> logger.warn(
        "[rime] 找不到可執行的 apksigner：$rimeApksigner。略過輪替重簽。"
    )

    else -> {
        val props: Properties = rimeSigningProps!!
        val lineage: File = rimeLineageFile!!

        fun resignWithLineage(apk: File) {
            val tmp = File(apk.parentFile, "${apk.name}.lineage-signed")
            val cmd = listOf(
                rimeApksigner.absolutePath, "sign",
                // 第一個簽章者 = lineage 的根（舊 debug 金鑰）。它的密碼是
                // Android 工具寫死的公開常數 "android"，不是秘密，可以直接帶。
                "--ks", props.getProperty("oldStoreFile"),
                "--ks-key-alias", props.getProperty("oldKeyAlias"),
                "--ks-pass", "pass:${props.getProperty("oldStorePassword")}",
                "--key-pass", "pass:${props.getProperty("oldKeyPassword")}",
                "--next-signer",
                // 第二個 = 正式金鑰。密碼走 file:，不進命令列 —— 命令列參數
                // 在多人共用的機器上任何人 `ps` 都看得到。
                // 兩個 file: 必須是**不同**檔案：apksigner 的 file: 取值是
                // 依序讀行的，共用一個檔會在第二次讀取時撞到檔尾。
                "--ks", props.getProperty("storeFile"),
                "--ks-key-alias", props.getProperty("keyAlias"),
                "--ks-pass", "file:${props.getProperty("storePassFile")}",
                "--key-pass", "file:${props.getProperty("keyPassFile")}",
                "--lineage", lineage.absolutePath,
                "--rotation-min-sdk-version", "28",
                "--v1-signing-enabled", "true",
                "--v2-signing-enabled", "true",
                "--v3-signing-enabled", "true",
                "--v4-signing-enabled", "false",
                "--in", apk.absolutePath,
                "--out", tmp.absolutePath,
            )
            val proc = ProcessBuilder(cmd).redirectErrorStream(true).start()
            val out = proc.inputStream.bufferedReader().readText()
            if (proc.waitFor() != 0) {
                tmp.delete()
                // 這裡**必須**讓建置失敗。悄悄產出一個升不上去的 APK，
                // 要到使用者裝不起來才會發現。
                throw GradleException("apksigner 重簽失敗（${apk.name}）：\n$out")
            }
            if (!tmp.renameTo(apk)) {
                tmp.copyTo(apk, overwrite = true)
                tmp.delete()
            }
            logger.lifecycle("[rime] 已帶 lineage 重簽：${apk.name}")
        }

        tasks.configureEach {
            val buildType = when (name) {
                "packageDebug" -> "debug"
                "packageRelease" -> "release"
                else -> null
            } ?: return@configureEach
            doLast {
                val dir = layout.buildDirectory.dir("outputs/apk/$buildType").get().asFile
                val apks = dir.listFiles { f: File -> f.isFile && f.extension == "apk" }.orEmpty()
                if (apks.isEmpty()) logger.warn("[rime] $dir 底下沒有 APK，無從重簽。")
                apks.forEach { resignWithLineage(it) }
            }
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.viewmodel)
    implementation(libs.androidx.savedstate)

    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.foundation)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.ui.tooling.preview)
    debugImplementation(libs.androidx.compose.ui.tooling)

    implementation(libs.androidx.datastore.preferences)

    testImplementation(libs.junit)
}
