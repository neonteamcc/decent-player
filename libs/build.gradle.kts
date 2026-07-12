plugins {
    id("com.android.library") version "9.1.0" apply false
}

// ── GitHub Packages publishing (all library modules) ────────────────
// CI passes -PpublishVersion=0.1.<run_number> on pushes to the release
// branch; the local default is a dev placeholder that never gets
// published. GitHub requires authentication even to READ public
// packages, so consumers need a PAT with read:packages.
subprojects {
    apply(plugin = "maven-publish")

    group = "cc.neonteam.decent"
    version = (findProperty("publishVersion") as String?) ?: "0.1.0-dev"

    plugins.withId("com.android.library") {
        extensions.configure<com.android.build.api.dsl.LibraryExtension>("android") {
            publishing {
                singleVariant("release")
            }
        }
        afterEvaluate {
            extensions.configure<PublishingExtension>("publishing") {
                publications {
                    create<MavenPublication>("release") {
                        from(components["release"])
                    }
                }
                repositories {
                    maven {
                        name = "GitHubPackages"
                        url = uri("https://maven.pkg.github.com/neonteamcc/decent-player")
                        credentials {
                            username = System.getenv("GITHUB_ACTOR") ?: findProperty("gpr.user") as String?
                            password = System.getenv("GITHUB_TOKEN") ?: findProperty("gpr.key") as String?
                        }
                    }
                }
            }
        }
    }
}
