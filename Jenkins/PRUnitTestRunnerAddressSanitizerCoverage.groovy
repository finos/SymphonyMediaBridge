@Library('SFE-RTC-pipeline') _

cmakeBuildType = "DCheck"

abortPreviousRunningBuilds()

notifyPRStatus("https://github.com/SymphonyOSF/rtc-smb", "Build and Unit Testing of ${env.BRANCH_NAME} [$cmakeBuildType]") {
    node('be-integration') {
        stage('Checkout') {
            checkout scm
        }
        prRunner = load('Jenkins/PRUnitTestRunner.groovy')
        prRunner.run(cmakeBuildType)
    }
}
