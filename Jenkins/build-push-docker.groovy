#!/usr/bin/env groovy

node {
    cleanWs()
    checkout scm
    def scmVars = checkout scm
    def gitHash = scmVars.GIT_COMMIT.substring(0,7)
    def gitBranch = scmVars.GIT_BRANCH.split('/')[-1]
    // Defaults to https://console.cloud.google.com/artifacts/docker/sym-dev-rtc/europe-west1/rtc-jenkins-tools?project=sym-dev-rtc
    def gcpArtifactsProject = params.GCP_ARTIFACTS_PROJECT ?: "sym-dev-rtc"
    def gcpArtifactsRegistry = params.GCP_ARTIFACTS_REGISTRY ?: "europe-west1-docker.pkg.dev"
    def gcpArtifactsRepo = params.GCP_ARTIFACTS_REPOSITORY ?: "rtc-jenkins-tools"

    def imageName = "buildsmb-${params.OSVERSION}"
    def gcpImageName="${gcpArtifactsRegistry}/${gcpArtifactsProject}/${gcpArtifactsRepo}/${imageName}"
    def githubImageName="ghcr.io/finos/symphonymediabridge/${imageName}"
    def gcpImageNameWithVer="${gcpImageName}:${gitHash}"
    def githubImageNameWithVer="${githubImageName}:${gitHash}"

    currentBuild.displayName = "${params.OSVERSION}-${gitBranch}"

    dir("./docker") {
        try {
            lock("jenkins-smb-image-build") {
                stage("Build") {
                    sh "./prepdocker.sh ${params.OSVERSION}"
                }
                stage("Push") {
                    sh "docker tag ${imageName}:latest ${gcpImageNameWithVer}"

                    if (params.GCP_KEY_FILE_PATH != null) {
                        sh "gcloud auth activate-service-account --key-file=${params.GCP_KEY_FILE_PATH}"
                    }
                    sh "gcloud auth configure-docker ${gcpArtifactsRegistry} --quiet"
                    sh "docker push ${gcpImageNameWithVer}"
                    sh "gcloud container images add-tag -q ${gcpImageNameWithVer} '${gcpImageName}:latest'"

                    withCredentials([
                        [$class: "UsernamePasswordMultiBinding", credentialsId: "ricardo-github-package-writer", usernameVariable: "GITGUB_USERNAME", passwordVariable: "GITHUB_PASSWORD"]
                    ]) {
                        sh "echo ${GITHUB_PASSWORD} | docker login ghcr.io -u ${GITGUB_USERNAME} --password-stdin"
                        sh "docker tag ${imageName}:latest ${githubImageNameWithVer}"
                        sh "docker tag ${imageName}:latest ${githubImageName}:latest"
                        sh "docker push --all-tags ${githubImageName}"
                    }

                    println "${gcpImageNameWithVer} successfully uploaded"
                }
            }
        } finally {
            stage("Cleanup") {
                sh "docker rmi ${imageName}"
                cleanWs()
            }
        }
    }
}
