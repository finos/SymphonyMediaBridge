@Library('SFE-RTC-pipeline') _

void prRunner(String cmakeBuildType, String platform, String dockerTag) {
    stage("Checkout") {
        checkout scm
    }

    stage("Build\n[$cmakeBuildType $platform]") {
        docker.image("gcr.io/sym-dev-rtc/buildsmb-$platform:$dockerTag").inside {
            env.GIT_COMMITTER_NAME = "Jenkins deployment job"
            env.GIT_COMMITTER_EMAIL = "jenkinsauto@symphony.com"
            sh "docker/$platform/buildscript.sh $cmakeBuildType"
            sh "mkdir $platform/$cmakeBuildType"
            sh "objdump -d $platform/smb/smb > $platform/$cmakeBuildType/smbobj.txt"
            sh "cp $platform/smb/smb $platform/$cmakeBuildType"
        }
    }
    stage("store artifacts") {
        archiveArtifacts artifacts: "$platform/$cmakeBuildType/smb, $platform/$cmakeBuildType/smbobj.txt", allowEmptyArchive: true
    }
    stage("Test\n[$cmakeBuildType $platform]") {
        docker.image("gcr.io/sym-dev-rtc/buildsmb-$platform:$dockerTag").inside {
            env.GIT_COMMITTER_NAME = "Jenkins deployment job"
            env.GIT_COMMITTER_EMAIL = "jenkinsauto@symphony.com"
            sh "docker/$platform/runtests.sh"
        }
    }
}

abortPreviousRunningBuilds()
// TODO: "Change back to be-integration before merge"
parallel "Release el7": {
    node('rtc-14160-test')
        prRunner("Release", "el7", "1f7ef85")
    }
}, "Release AWS-linux": {
    node('rtc-14160-test')
        prRunner("Release", "aws-linux", "latest")
    }
}, "Release el8": {
    node('rtc-14160-test')
        prRunner("Release", "el8", "latest")
    }
}, "LCheck": {
    node('rtc-14160-test')
        prRunner("LCheck", "el8", "latest")
    }
}, "TCheck": {
    node('rtc-14160-test')
        prRunner("TCheck", "el8", "latest")
    }
}, "DCheck": {
    node('rtc-14160-test')
        prRunner("DCheck", "el8", "latest")
    }
}, "LCov": {
    node('rtc-14160-test')
        try {
            prRunner("LCov", "el8", "latest")
        } finally {
            stage("Post Actions") {
                dir ("el8/smb") {
                    junit testResults: "test-results*.xml"
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
    node('rtc-14160-test')
        prRunner("Release", "ubuntu-focal-deb", "latest")
    }
}
