@Library('SFE-RTC-pipeline') _

void prRunner(String cmakeBuildType, String platform, String dockerTag) {
    stage("Checkout") {
        checkout scm
    }

    stage("Build and test\n[$cmakeBuildType $platform]") {
        docker.image("gcr.io/sym-dev-rtc/buildsmb-$platform:$dockerTag").inside {
            env.GIT_COMMITTER_NAME = "Jenkins deployment job"
            env.GIT_COMMITTER_EMAIL = "jenkinsauto@symphony.com"
            sh "docker/$platform/buildscript.sh $cmakeBuildType"
            sh "docker/$platform/runtests.sh"
        }
    }

    stage("artifacts") {
        sh "objdump -d smb > smbobj.txt"
        archiveArtifacts artifacts: smb, smbobj.txt
    }
}

abortPreviousRunningBuilds()

parallel "Release el7": {
    node('be-integration') {
        prRunner("Release", "el7", "1f7ef85")
    }
}, "Release AWS-linux": {
    node('be-integration') {
        prRunner("Release", "aws-linux", "latest")
    }
}, "Release el8": {
    node('be-integration') {
        prRunner("Release", "el8", "latest")
    }
}, "LCheck": {
    node('be-integration') {
        prRunner("LCheck", "el8", "latest")
    }
}, "TCheck": {
    node('be-integration') {
        prRunner("TCheck", "el8", "latest")
    }
}, "DCheck": {
    node('be-integration') {
        prRunner("DCheck", "el8", "latest")
        archiveArtifacts artifacts: smb
    }
}, "LCov": {
    node('be-integration') {
        try {
            prRunner("LCov", "el8", "latest")
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
        prRunner("Release", "ubuntu-focal-deb", "latest")
    }
}
