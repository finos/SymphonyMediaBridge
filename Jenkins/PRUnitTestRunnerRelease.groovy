@Library('SFE-RTC-pipeline@marcus/sha_check') _

void prRunner(String cmakeBuildType) {
    docker.image('gcr.io/sym-dev-rtc/buildsmb-ubuntu-focal:latest').inside {
        stage("Build ubuntu-focal [$cmakeBuildType]") {
            env.GIT_COMMITTER_NAME = "Jenkins deployment job"
            env.GIT_COMMITTER_EMAIL = "jenkinsauto@symphony.com"
            sh "docker/ubuntu-focal/buildscript.sh $cmakeBuildType"
        }
    }

    try {
        docker.image('gcr.io/sym-dev-rtc/buildsmb-ubuntu-focal:latest').inside {
            stage('Run tests centos7') {
                sh "docker/el7/runtests.sh"
            }
        }
    } finally {
        stage("Post Actions") {
            dir ("ubuntu-focal/smb") {
                junit testResults: "test-results.xml"

                hasCoverage = (cmakeBuildType == "DCheck");
                if (hasCoverage) {
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

abortPreviousRunningBuilds()

node('be-integration') {
    stage('Checkout') {
        checkout scm
    }

    stage('Release') {
        prRunner("Release")
    }

    stage('DCheck') {
        prRunner("DCheck")
    }

    stage('LCheck') {
        prRunner("LCheck")
    }

    stage('TCheck') {
        prRunner("TCheck")
    }
}
