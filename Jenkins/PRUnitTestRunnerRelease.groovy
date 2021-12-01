@Library('SFE-RTC-pipeline@marcus/sha_check') _

void prRunner(String cmakeBuildType) {
    checkout scm

    docker.image('gcr.io/sym-dev-rtc/buildsmb-ubuntu-focal:latest').inside {
        env.GIT_COMMITTER_NAME = "Jenkins deployment job"
        env.GIT_COMMITTER_EMAIL = "jenkinsauto@symphony.com"
        sh "docker/ubuntu-focal/buildscript.sh $cmakeBuildType"
        sh "docker/ubuntu-focal/runtests.sh"
    }
}

abortPreviousRunningBuilds()

stages {
    parallel {
        stage("Release") {
            node('be-integration') {
                prRunner("Release")
            }
        }
        stage("LCheck") {
            node('be-integration') {
                prRunner("LCheck")
            }
        }
        stage("TCheck") {
            node('be-integration') {
                prRunner("TCheck")
            }
        }
        stage("DCheck") {
            node('be-integration') {
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
        }
    }
}
