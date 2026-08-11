plugins {
    id("com.android.library")
}

android {
    namespace = "com.decent.audio"
    compileSdk = 36

    defaultConfig {
        // Matches the app's floor. A library floor above it would ship a
        // download pipeline that some supported devices cannot load at all —
        // build-ffmpeg.sh compiles the FFmpeg libraries against API 28 for the
        // same reason.
        minSdk = 28

        externalNativeBuild {
            cmake {
                targets += "flowyaudio"
            }
        }
        // Stated rather than defaulted: the wrapper can only be built for an
        // ABI that build-ffmpeg.sh produced, and this is exactly that set.
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64", "x86")
        }

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        // The keep rules ride INSIDE the AAR (they land as proguard.txt), so an
        // app that adds this dependency inherits them and cannot forget them.
        // The native code constructs FlowyFfmpeg$AudioInfo by name; a consumer
        // minifying without these rules gets a release-only crash on the first
        // probe() and nothing at all in debug.
        consumerProguardFiles("consumer-rules.pro")
    }

    ndkVersion = "29.0.14206865"

    externalNativeBuild {
        cmake {
            path("src/main/jni/CMakeLists.txt")
            version = "3.21.0+"
        }
    }

    // The prebuilt FFmpeg libraries and the LGPL notices are staged into the
    // module by the tasks below; both directories are build output and are
    // gitignored.
    sourceSets["main"].jniLibs.srcDirs("prebuilt-jnilibs")
    sourceSets["main"].assets.srcDirs("prebuilt-assets")

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
}

dependencies {
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test:runner:1.6.2")
}

// build-ffmpeg.sh installs into prebuilt/<abi>/lib/*.so, but the AAR packages
// jniLibs as <abi>/*.so — the `lib/` level has to come out.
val stageJniLibs by tasks.registering(Copy::class) {
    from("prebuilt") {
        include("*/lib/*.so")
        eachFile { path = "${path.substringBefore('/')}/$name" }
    }
    into("prebuilt-jnilibs")
    includeEmptyDirs = false
}

// These libraries are shared precisely so that the LGPL's relinking
// requirement is satisfied by construction; the notice is the other half of
// that obligation, so it has to travel inside the AAR and on into the APK —
// not merely exist in a gitignored build tree.
val stageLicenses by tasks.registering(Copy::class) {
    from("prebuilt") { include("COPYING.*") }
    into("prebuilt-assets/licenses")
    includeEmptyDirs = false
}

tasks.named("preBuild") {
    dependsOn(stageJniLibs, stageLicenses)
}
