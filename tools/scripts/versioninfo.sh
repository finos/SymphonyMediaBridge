#!/bin/bash

function getVersion {
    if [ -z "${BUILD_NUMBER}" ]; then
        echo "localbuild"
    else
        echo "${VERSION}"
    fi
}

function getBranch {
    if [ -z "${GIT_BRANCH}" ]; then
        echo "$(git rev-parse --abbrev-ref HEAD)"
    else
        echo "${GIT_BRANCH}"
    fi
}

function getCommitHash {
    if [ -z "${GIT_COMMIT}" ]; then
        echo "$(git rev-parse HEAD)"
    else
        echo "${GIT_COMMIT}"
    fi
}

function getJenkinsUrl {
    if [ -z "${BUILD_URL}" ]; then
        echo "n/a"
    else
        echo "${BUILD_URL}"
    fi
}

echo "version    : $(getVersion)" > versioninfo.txt
echo "branch     : $(getBranch)" >> versioninfo.txt
echo "commit hash: $(getCommitHash)" >> versioninfo.txt
