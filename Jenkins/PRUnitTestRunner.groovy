@Library('SFE-RTC-pipeline') _

void prRunner(String cmakeBuildType, String platform) {
    stage("Checkout") {
        checkout scm
    }

    stage("Build and test") {
        docker.image("gcr.io/sym-dev-rtc/buildsmb-$platform:latest").inside {
            env.GIT_COMMITTER_NAME = "Jenkins deployment job"
            env.GIT_COMMITTER_EMAIL = "jenkinsauto@symphony.com"
            sh "docker/$platform/buildscript.sh $cmakeBuildType"
            sh "docker/$platform/runtests.sh"
        }
    }
}

abortPreviousRunningBuilds()

parallel "Release el7": {
    node('be-integration') {
        prRunner("Release", "el7")
    }
}, "LCheck": {
    node('be-integration') {
        prRunner("LCheck", "ubuntu-focal-deb")
    }
}, "TCheck": {
    node('be-integration') {
        prRunner("TCheck", "ubuntu-focal-deb")
    }
}, "DCheck": {
    node('be-integration') {
        try {
            prRunner("DCheck", "ubuntu-focal-deb")
        } finally {
            stage("Post Actions") {
                dir ("ubuntu-focal-deb/smb") {
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
}, "Release Ubuntu public": {
    node('be-integration') {
        prRunner("Release", "ubuntu-focal-deb")
    }
}
