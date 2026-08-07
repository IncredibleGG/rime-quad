import org.jetbrains.kotlin.gradle.dsl.JvmTarget

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

android {
    namespace = rimeNamespace
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = rimeApplicationId
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0-dev"

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

    buildTypes {
        debug {
            isJniDebuggable = true
        }
        release {
            isMinifyEnabled = false
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
