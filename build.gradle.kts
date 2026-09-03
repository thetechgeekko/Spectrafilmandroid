// Spektrafilm for Android — root build. GPLv3.
import org.gradle.api.artifacts.dsl.LockMode

plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.android.library) apply false
    alias(libs.plugins.kotlin.android) apply false
    alias(libs.plugins.kotlin.compose) apply false
}

// Every configuration exercised by release qualification is dependency-locked,
// including project-local JVM tests. Missing/stale state fails instead of
// silently resolving a different graph.
allprojects {
    dependencyLocking {
        lockAllConfigurations()
        lockMode.set(LockMode.STRICT)
    }
}
