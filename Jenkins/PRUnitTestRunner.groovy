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
//#recipientProviders: [[$class: 'CulpritsRecipientProvider']]
// maybe remove to
    post {
        unsuccessful {
            emailext (
                recipientProviders: [requestor()],
                subject: "Jenkins PR build failed",
                body: """
                    <p>
                    <h2><a href=\"${env.BUILD_URL}\">Jenkins PR build failed #${env.BUILD_NUMBER} for SMB.</a></h2>
                    </p>
                    """,
                mimeType: 'text/html',
                attachLog: true
            )
        }
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
    }
}, "LCov": {
    node('be-integration') {
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
    node('be-integration') {
        prRunner("Release", "ubuntu-focal-deb", "latest")
    }
}
