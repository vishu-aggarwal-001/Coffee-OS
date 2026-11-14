plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.freshground.kiosk"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.freshground.kiosk"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "1.0.0"
        // Address of the coffee machine controller on its private Wi-Fi
        buildConfigField("String", "MACHINE_URL", "\"http://192.168.4.1/\"")
    }
    buildFeatures { buildConfig = true }
    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")   // replace with your own key before wide rollout
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}
