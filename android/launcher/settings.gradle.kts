pluginManagement {
    repositories { google(); mavenCentral(); gradlePluginPortal() }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories { google(); mavenCentral() }
}
rootProject.name = "ZettaBridge"
include(":app")
include(":manager")
include(":step04probe")
include(":step05bootstrap")
include(":step05fixture")
include(":step06fixture")
include(":step08fixture")
include(":step09fixture")
