void prRunner(String cmakeBuildType) {
    stage("Checkout") {
        checkout scm
    }

    stage("Build and test") {
        docker.image('gcr.io/sym-dev-rtc/buildsmb-el7:latest').inside {
            env.GIT_COMMITTER_NAME = "Jenkins deployment job"
            env.GIT_COMMITTER_EMAIL = "jenkinsauto@symphony.com"
            sh "docker/el7/buildscript.sh $cmakeBuildType"
            sh "docker/el7/runtests.sh"
        }
    }
}

abortPreviousRunningBuilds()

parallel "Release": {
    node('be-integration') {
        prRunner("Release")
    }
}, "LCheck": {
    node('be-integration') {
        prRunner("LCheck")
    }
}, "TCheck": {
    node('be-integration') {
        prRunner("TCheck")
    }
}, "DCheck": {
    node('be-integration') {
        try {
            prRunner("DCheck")
        } finally {
            stage("Post Actions") {
                dir ("el7/smb") {
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
