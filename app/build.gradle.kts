plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    jacoco
}

android {
    namespace = "com.hamanpaul.liukai"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.hamanpaul.liukai"
        minSdk = 30
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
    }

    buildTypes {
        debug {
            // debug APK 以 JaCoCo offline instrumentation 建置，供 TestPilot 端對端案例量測 app 覆蓋率
            enableAndroidTestCoverage = true
        }
        release {
            isMinifyEnabled = false
        }
    }

    testCoverage {
        jacocoVersion = "0.8.15"
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

jacoco {
    toolVersion = "0.8.15"
}

// app 覆蓋率：TestPilot 案例在模擬器上執行後匯出的 .ec（放在 build/outputs/e2e-coverage/）。
// 門檻為行與分支 100%（見 docs/superpowers/plans/2026-09-26-testpilot-full-coverage.md）。
val e2eCoverageData = fileTree(layout.buildDirectory.dir("outputs/e2e-coverage")) { include("*.ec") }
val e2eClasses = fileTree(layout.buildDirectory.dir("tmp/kotlin-classes/debug")) {
    exclude("**/R.class", "**/R$*.class", "**/BuildConfig.class")
}

val jacocoE2eReport by tasks.registering(JacocoReport::class) {
    executionData.setFrom(e2eCoverageData)
    classDirectories.setFrom(e2eClasses)
    sourceDirectories.setFrom(files("src/main/kotlin", "src/debug/kotlin"))
    reports {
        xml.required.set(true)
        html.required.set(true)
    }
}

val jacocoE2eCoverageVerification by tasks.registering(JacocoCoverageVerification::class) {
    executionData.setFrom(e2eCoverageData)
    classDirectories.setFrom(e2eClasses)
    sourceDirectories.setFrom(files("src/main/kotlin", "src/debug/kotlin"))
    violationRules {
        rule {
            limit {
                counter = "LINE"
                minimum = "1.0".toBigDecimal()
            }
            limit {
                counter = "BRANCH"
                minimum = "1.0".toBigDecimal()
            }
        }
    }
}
