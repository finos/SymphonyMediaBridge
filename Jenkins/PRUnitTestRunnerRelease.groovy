@Library('SFE-RTC-pipeline@marcus/sha_check') _

void prRunner(String cmakeBuildType) {
    docker.image('gcr.io/sym-dev-rtc/buildsmb-ubuntu-focal:latest').inside {
        stage("Build ubuntu-focal [$cmakeBuildType]") {
            env.GIT_COMMITTER_NAME = "Jenkins deployment job"
            env.GIT_COMMITTER_EMAIL = "jenkinsauto@symphony.com"
            sh "docker/ubuntu-focal/buildscript.sh $cmakeBuildType"
        }

        stage('Run tests ubuntu-focal') {
            sh "docker/ubuntu-focal/runtests.sh"
        }
    }
}

abortPreviousRunningBuilds()

node('be-integration') {
    stage('Checkout') {
        checkout scm
    }

    prRunner("Release")
    prRunner("LCheck")
    prRunner("TCheck")

    try {
        prRunner("DCheck")
    } finally {
        stage("Post Actions") {
            dir ("ubuntu-focal/smb") {
                junit testResults: "test-results.xml"
                publishHTML(target: [
                        allowMissing         : false,
                        alwaysLinkToLastBuild: false,
                        keepAll              : true,
                        reportDir            : "coverage",
                        reportFiles          : "index.html",
                        reportName           : "Code Coverage Report"
                ])
            }
        }
    }
}
