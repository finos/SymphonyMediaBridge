@Library('SFE-RTC-pipeline@marcus/sha_check') _

cmakeBuildType = "TCheck"

abortPreviousRunningBuilds()

notifyPRStatus("https://github.com/finos/SymphonyMediaBridge", "Build and Unit Testing of ${env.BRANCH_NAME} [$cmakeBuildType]") {
    node('be-integration') {
        stage('Checkout') {
            checkout scm
        }
        prRunner = load('Jenkins/PRUnitTestRunner.groovy')
        prRunner.run(cmakeBuildType)
    }
}
