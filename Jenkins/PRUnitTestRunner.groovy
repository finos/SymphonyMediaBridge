void run(String cmakeBuildType) {
    docker.image('gcr.io/sym-dev-rtc/buildsmb-ubuntu-focal-deb:latest').inside {
        stage("Build Ubuntu Focal public release [$cmakeBuildType]") {
            env.GIT_COMMITTER_NAME = "Jenkins deployment job"
            env.GIT_COMMITTER_EMAIL = "jenkinsauto@symphony.com"
            sh "docker/ubuntu-focal-deb/buildscript.sh $cmakeBuildType"
        }
    }

    docker.image('gcr.io/sym-dev-rtc/buildsmb-el7:latest').inside {
        stage("Build Centos 7 [$cmakeBuildType]") {
            env.GIT_COMMITTER_NAME = "Jenkins deployment job"
            env.GIT_COMMITTER_EMAIL = "jenkinsauto@symphony.com"
            sh "docker/el7/buildscript.sh $cmakeBuildType"
        }
    }

    try {
        docker.image('gcr.io/sym-dev-rtc/buildsmb-el7:latest').inside {
            stage('Run tests Centos 7') {
                sh "docker/el7/runtests.sh"
            }
        }
        docker.image('gcr.io/sym-dev-rtc/buildsmb-ubuntu-focal-deb:latest').inside {
            stage('Run tests Ubuntu Focal public release') {
                sh "docker/ubuntu-focal-deb/runtests.sh"
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
            dir ("ubuntu-focal-deb/smb") {
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
