@Library('SFE-RTC-pipeline') _

void prRunner(String cmakeBuildType, String platform) {
    stage("Checkout") {
        checkout scm
    }

    stage("Build and test\n[$cmakeBuildType $platform]") {
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
        prRunner("LCheck", "el8")
    }
}, "TCheck": {
    node('be-integration') {
        prRunner("TCheck", "el8")
    }
}, "DCheck": {
    node('be-integration') {
        try {
            prRunner("DCheck", "el8")
        } finally {
            stage("Post Actions") {
                dir ("el8/smb") {
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
