plugins {
    id("org.jetbrains.kotlin.jvm")
    application
}

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

application {
    mainClass.set("com.hamanpaul.liukai.cli.MainKt")
    applicationName = "liu-kai-cli"
}

dependencies {
    implementation(project(":core"))
    testImplementation(kotlin("test-junit"))
    testImplementation("junit:junit:4.13.2")
}
