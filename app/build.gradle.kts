plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.hamanpaul.liukai"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.hamanpaul.liukai"
        minSdk = 28
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        buildConfig = true
    }

    // debug 版內建合成字表（與 core 單元測試共用同一份 fixture），供模擬器 E2E 使用。
    sourceSets["debug"].assets.srcDir("../core/src/test/resources/fixtures")
}

dependencies {
    implementation(project(":core"))
}
