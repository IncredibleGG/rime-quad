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
}
