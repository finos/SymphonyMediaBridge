void run(String cmakeBuildType) {
    docker.image('gcr.io/sym-dev-rtc/buildsmb7:latest').inside {
        stage("Build centos7 [$cmakeBuildType]") {
            env.GIT_COMMITTER_NAME = "Jenkins deployment job"
            env.GIT_COMMITTER_EMAIL = "jenkinsauto@symphony.com"
            sh "docker/el7/buildscript.sh $cmakeBuildType"
        }
    }

    try {
        docker.image('gcr.io/sym-dev-rtc/buildsmb7:latest').inside {
            stage('Run tests centos7') {
                sh "docker/el7/runtests.sh"
            }
        }
    } finally {
        stage("Post Actions") {
            dir ("el7/smb") {
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

return this
