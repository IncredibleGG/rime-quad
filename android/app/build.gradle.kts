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
// 隨附執行期資料（schema、詞庫、opencc、essay 語言模型）。
//
// 這些檔案由 scripts/collect_data.sh 產生，體積 13MB，且**刻意不進版控**
// （見專案根目錄 .gitignore 的 /core/data/）。所以這裡不把它們複製一份到
// src/main/assets 提交上去，而是在建置時同步進 build/ 底下的 generated
// assets 目錄 —— 產生物留在產生物該在的地方。
//
// 沒跑過 collect_data.sh 的機器一樣建置得起來，只是 APK 內沒有 schema，
// 執行期 librime 部署會失敗；下面的 doFirst 會先警告。
// ─────────────────────────────────────────────────────────────────────────────
val rimeRepoRoot = layout.projectDirectory.dir("../..")
val rimeSharedData = rimeRepoRoot.dir("core/data/shared")
val rimeUserData = rimeRepoRoot.dir("core/data/user")
val rimeGeneratedAssets = layout.buildDirectory.dir("generated/rimeAssets")

val syncRimeData = tasks.register<Sync>("syncRimeData") {
    description = "把 core/data 的隨附資料同步進 generated assets"
    into(rimeGeneratedAssets)
    from(rimeSharedData) { into("rime/shared") }
    from(rimeUserData) { into("rime/user") }
    doFirst {
        if (!rimeSharedData.asFile.isDirectory) {
            logger.warn(
                "[rime] 找不到 ${rimeSharedData.asFile}，APK 將不含任何 schema。" +
                    " 先跑 scripts/collect_data.sh。"
            )
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

    testImplementation(libs.junit)
}
