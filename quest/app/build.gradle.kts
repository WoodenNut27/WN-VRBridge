plugins {
    id("com.android.application")
}

// 版はリポジトリのルートの VERSION ファイルだけに書く（PC の CMake と配布スクリプトも同じファイルを読む）。
val appVersion = rootProject.file("../VERSION").readText().trim()

android {
    namespace = "com.woodennut27.wnvrbridge"
    compileSdk = 34
    ndkVersion = "30.0.16248370"

    defaultConfig {
        applicationId = "com.woodennut27.wnvrbridge"
        minSdk = 32
        targetSdk = 34
        versionCode = 1
        versionName = appVersion
        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_shared"
            }
        }
    }
    buildTypes {
        getByName("debug") {
            externalNativeBuild {
                cmake {
                    // 実機で遅延とスループットを測るため、デバッグ版でもネイティブのコードは最適化する。
                    arguments += "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
                }
            }
        }
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }
    buildFeatures {
        prefab = true
    }
}

dependencies {
    implementation("org.khronos.openxr:openxr_loader_for_android:1.1.65")
}
