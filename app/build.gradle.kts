import javax.inject.Inject

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    jacoco
}

val doctorSourceProperty = providers.gradleProperty("liukai.doctorSourceCommit")
val distributionBuild = providers.gradleProperty("liukai.signForDistribution")
val doctorGitHead = providers.exec {
    workingDir(rootDir)
    commandLine("git", "rev-parse", "HEAD")
}.standardOutput.asText.map(String::trim)
val doctorSourceCommit = when {
    doctorSourceProperty.isPresent -> doctorSourceProperty.get()
    distributionBuild.isPresent -> doctorGitHead.get()
    else -> "0000000"
}
val doctorSourceCommitOrigin = when {
    doctorSourceProperty.isPresent -> "gradle-property"
    distributionBuild.isPresent -> "distribution-git-head"
    else -> "development-synthetic"
}
if (!doctorSourceCommit.matches(Regex("[0-9a-f]{7,40}"))) {
    throw GradleException(
        "liukai.doctorSourceCommit must be 7..40 lowercase hexadecimal characters",
    )
}

android {
    namespace = "com.hamanpaul.liukai"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.hamanpaul.liukai"
        minSdk = 30
        targetSdk = 35
        versionCode = file("VERSION_CODE").readText().trim().toInt()
        versionName = file("VERSION").readText().trim()
        testInstrumentationRunner = "com.hamanpaul.liukai.ime.TouchDiagnosticsFrameworkTest"
        buildConfigField("String", "DOCTOR_SOURCE_COMMIT", "\"$doctorSourceCommit\"")
        buildConfigField("String", "DOCTOR_SOURCE_COMMIT_ORIGIN", "\"$doctorSourceCommitOrigin\"")
    }

    buildTypes {
        debug {
            // debug APK 以 JaCoCo offline instrumentation 建置，供 TestPilot 端對端案例量測 app 覆蓋率
            enableAndroidTestCoverage = true
        }
        release {
            isMinifyEnabled = false
            // Public APKs use this historical signing identity for in-place upgrades.
            // The release entry point verifies the public certificate before distribution.
            if (providers.gradleProperty("liukai.signForDistribution").isPresent) {
                signingConfig = signingConfigs.getByName("debug")
            }
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
    // Run the same pure Doctor regression tests on ART for device coverage.
    sourceSets["androidTest"].java.srcDir("src/test/kotlin")
}

// 內建字表：本機有使用者自建字表時（預設 ~/prj_pri/liu-kai-data，可用 -Pliukai.tableDir 或環境變數
// LIU_KAI_DATA 指定），建置前以 liu-kai-cli bundle 編譯成 assets/bundled.liutable 內建進 APK。
// 字表原始檔不進 git；沒有字表時（例如 CI）略過，APK 不內建字表，需在設定頁匯入。
abstract class BundleTableTask @Inject constructor(private val execOps: ExecOperations) : DefaultTask() {
    @get:InputFiles
    abstract val sources: ConfigurableFileCollection

    @get:Classpath
    abstract val cliClasspath: ConfigurableFileCollection

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @TaskAction
    fun bundle() {
        val dir = outputDir.get().asFile
        dir.deleteRecursively()
        dir.mkdirs()
        val files = sources.files.toList()
        if (!files.all { it.exists() }) {
            logger.lifecycle("找不到字表 ${files.joinToString()}：APK 不內建字表")
            return
        }
        execOps.javaexec {
            classpath = cliClasspath
            mainClass.set("com.hamanpaul.liukai.cli.MainKt")
            args(listOf("bundle") + files.map { it.path } + listOf("--out", File(dir, "bundled.liutable").path))
        }
    }
}

val cliRuntime: Configuration by configurations.creating

dependencies {
    implementation(project(":core"))
    cliRuntime(project(":cli"))
    testImplementation(kotlin("test-junit"))
    testImplementation("junit:junit:4.13.2")
    androidTestImplementation(kotlin("test-junit"))
    androidTestImplementation("junit:junit:4.13.2")
}

val tableDir = providers.gradleProperty("liukai.tableDir")
    .orElse(providers.environmentVariable("LIU_KAI_DATA"))
    .orElse(System.getProperty("user.home") + "/prj_pri/liu-kai-data")

val bundleTable by tasks.registering(BundleTableTask::class) {
    sources.from(tableDir.map { dir -> listOf("liu_ibus_final.txt", "lime_liu7.txt").map { File(dir, it) } })
    cliClasspath.from(cliRuntime)
    outputDir.set(layout.buildDirectory.dir("generated/liukai-table"))
}

androidComponents {
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(bundleTable, BundleTableTask::outputDir)
    }
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
