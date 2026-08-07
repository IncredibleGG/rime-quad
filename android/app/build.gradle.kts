import org.jetbrains.kotlin.gradle.dsl.JvmTarget
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
// 沒跑過 collect_data.sh 的機器一樣建置得起來，只是 APK 內沒有 schema，
// 執行期 librime 部署會失敗；下面的 doFirst 會先警告。
// ─────────────────────────────────────────────────────────────────────────────
val rimeRepoRoot = layout.projectDirectory.dir("../..")
val rimeSharedData = rimeRepoRoot.dir("core/data/shared")
val rimeUserData = rimeRepoRoot.dir("core/data/user")
val rimeLayouts = rimeRepoRoot.dir("core/layouts")
val rimeThemes = rimeRepoRoot.dir("core/themes")
val rimeSchemaLanguages = rimeRepoRoot.file("core/schema-languages.json")
val rimeGeneratedAssets = layout.buildDirectory.dir("generated/rimeAssets")

val syncRimeData = tasks.register<Sync>("syncRimeData") {
    description = "把 core/data、core/layouts、core/themes 同步進 generated assets"
    into(rimeGeneratedAssets)
    from(rimeSharedData) { into("rime/shared") }
    from(rimeUserData) { into("rime/user") }
    // 佈局與主題：assets 內的路徑刻意與 docs/theme-format.md §2.3 的
    // 「隨附目錄」同名，四端的搜尋路徑因此長得一樣。
    from(rimeLayouts) { into("rime/layouts") }
    from(rimeThemes) { into("rime/themes") }
    // 方案 → BCP 47 語言標記的對照表（scripts/schema_store/languages.py 產生）。
    // 選單分組讀的是這一份加上索引裡的 language 欄位；兩者都沒有時才退回啟發式。
    from(rimeSchemaLanguages) { into("rime") }
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
        // 為什麼 debug 也要：使用者手上跑的就是 debug 建置（本專案側載發布的
        // 是 debug APK）。debug 若還用 Android 的 debug keystore 簽，等於「能不能
        // 升級」取決於他當初裝到哪一種建置 —— 那是個會在最糟的時候才發現的坑。
        val rimeSigning = signingConfigs.findByName("rime")
        debug {
            isJniDebuggable = true
            rimeSigning?.let { signingConfig = it }
        }
        release {
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
}

// assets 合併之前必須先同步完資料。
tasks.matching { it.name.startsWith("merge") && it.name.endsWith("Assets") }
    .configureEach { dependsOn(syncRimeData) }
tasks.named("preBuild") { dependsOn(syncRimeData) }

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
