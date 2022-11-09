#!/usr/bin/env groovy

node {
    cleanWs()
    checkout scm
    scmVars = checkout scm
    git_hash = scmVars.GIT_COMMIT.substring(0,7)

    dir("./") {
        try {
            stage("Build") {
                sh "./docker/build_loadtest_container.sh"
            }
            stage("Push") {
                gcloud_image_name="gcr.io/$params.GCE_PROJECT_ID/buildsmb-loadtests"
                gcloud_image_name_with_ver="$gcloud_image_name:$git_hash"
                sh "docker tag buildsmb-loadtests:latest $gcloud_image_name_with_ver"
                if (params.GCE_KEY_FILE_PATH != null) {
                    sh "gcloud auth activate-service-account --key-file=$params.GCE_KEY_FILE_PATH"
                }
                // TODO: Remove the call for gcloud beta when https://warpdrive-lab.dev.symphony.com/jenkins has up to date gcloud tools
                sh "gcloud auth configure-docker || gcloud beta auth configure-docker"
                sh "docker push $gcloud_image_name_with_ver"
                sh "gcloud container images add-tag -q $gcloud_image_name_with_ver '$gcloud_image_name:latest'"
                sh "echo $gcloud_image_name_with_ver successfully uploaded"
            }
        } finally {
            stage("Cleanup") {
                sh "docker rmi buildsmb-loadtests || true"
                cleanWs()
            }
        }
    }
}
