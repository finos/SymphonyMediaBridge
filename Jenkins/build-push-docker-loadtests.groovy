#!/usr/bin/env groovy

node {
    cleanWs()
    checkout scm
    def scmVars = checkout scm
    def gitHash = scmVars.GIT_COMMIT.substring(0,7)
    // Defaults to https://console.cloud.google.com/artifacts/docker/sym-dev-rtc/europe-west1/rtc-jenkins-tools?project=sym-dev-rtc
    def gcpArtifactsProject = params.GCP_ARTIFACTS_PROJECT ?: "sym-dev-rtc"
    def gcpArtifactsRegistry = params.GCP_ARTIFACTS_REGISTRY ?: "europe-west1-docker.pkg.dev"
    def gcpArtifactsRepo = params.GCP_ARTIFACTS_REPOSITORY ?: "rtc-jenkins-tools"

    def imageName = params.IMAGE_NAME ?: "buildsmb-loadtests"
    def gcpImageName="${gcpArtifactsRegistry}/${gcpArtifactsProject}/${gcpArtifactsRepo}/${imageName}"
    def gcpImageNameWithVer="${gcpImageName}:${gitHash}"

    dir("./") {
        try {
            stage("Build") {
                sh "./docker/build_loadtest_container.sh"
            }
            stage("Push") {
                sh "docker tag ${imageName}:latest ${gcpImageNameWithVer}"
                if (params.GCP_KEY_FILE_PATH != null) {
                    sh "gcloud auth activate-service-account --key-file=${params.GCP_KEY_FILE_PATH}"
                }
                sh "gcloud auth configure-docker ${gcpArtifactsRegistry} --quiet"
                sh "docker push ${gcpImageNameWithVer}"
                sh "gcloud container images add-tag -q ${gcpImageNameWithVer} '${gcpImageName}:latest'"
                println "${gcpImageNameWithVer} successfully uploaded"
            }
        } finally {
            stage("Cleanup") {
                sh "docker rmi ${imageName}"
                cleanWs()
            }
        }
    }
}
