plugins {
    id("com.android.application")
    id("kotlin-android")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

android {
    namespace = "com.lumacore.lumacore"
    compileSdk = flutter.compileSdkVersion
    // Pinned to an already-installed NDK (see dev environment: 25.1/26.3/27.0)
    // instead of flutter.ndkVersion — that resolved to 28.2.13676358, which
    // isn't installed and triggers a slow first-build SDK Manager download
    // for no benefit here (our externalNativeBuild only needs a working
    // arm64-v8a toolchain, not Flutter's specific recommended NDK).
    ndkVersion = "27.0.12077973"

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    kotlinOptions {
        jvmTarget = JavaVersion.VERSION_11.toString()
    }

    defaultConfig {
        // TODO: Specify your own unique Application ID (https://developer.android.com/studio/build/application-id.html).
        applicationId = "com.lumacore.lumacore"
        // You can update the following values to match your application needs.
        // For more information, see: https://flutter.dev/to/review-gradle-config.
        // minSdk pinned above flutter.minSdkVersion: MediaStore's scoped-storage
        // add-only save path (RecordingController.kt) needs API 29, and we
        // haven't built the API 24-28 WRITE_EXTERNAL_STORAGE fallback yet (see
        // ai_plans/04-android-camerax-gl-pipeline.md §D.2) — 24 is still below
        // that, kept as the floor this round, not the target.
        minSdk = maxOf(flutter.minSdkVersion, 24)
        targetSdk = flutter.targetSdkVersion
        versionCode = flutter.versionCode
        versionName = flutter.versionName

        // Only arm64-v8a is built (emulator + all current devices) — same
        // arm64-only scope as the iOS build (see ai_plans/04 context
        // section). Two separate abiFilters exist in AGP: ndk.abiFilters
        // only controls which .so's get *packaged*; the CMake invocation
        // itself is governed by externalNativeBuild.cmake.abiFilters here —
        // without restricting that one too, CMake still configures for
        // every default ABI (armeabi-v7a included) and hits native/
        // CMakeLists.txt's `message(FATAL_ERROR ...)` arm64-only guard.
        // Both are pre-populated by the Flutter Gradle plugin, so clear()
        // first rather than `+=` onto the existing set.
        ndk {
            abiFilters.clear()
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                abiFilters.clear()
                abiFilters += "arm64-v8a"
                arguments += "-DANDROID_STL=c++_shared"
            }
        }
    }

    externalNativeBuild {
        // Points directly at the shared native/CMakeLists.txt — no second,
        // Android-only CMakeLists.txt (ARCHITECTURE.md §5).
        cmake {
            path = file("../../native/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            // TODO: Add your own signing config for the release build.
            // Signing with the debug keys for now, so `flutter run --release` works.
            signingConfig = signingConfigs.getByName("debug")
        }
    }
}

dependencies {
    // CameraX (Preview use case feeds the camera-input GL_TEXTURE_EXTERNAL_OES
    // texture — see CameraCaptureController.kt) + lifecycle-runtime (plain
    // FlutterActivity has no Lifecycle of its own; MainActivity implements
    // LifecycleOwner by hand with LifecycleRegistry, which needs this).
    val cameraxVersion = "1.3.4"
    implementation("androidx.camera:camera-core:$cameraxVersion")
    implementation("androidx.camera:camera-camera2:$cameraxVersion")
    implementation("androidx.camera:camera-lifecycle:$cameraxVersion")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.7.0")
}

flutter {
    source = "../.."
}
