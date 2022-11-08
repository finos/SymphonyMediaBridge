#!/bin/bash
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
echo "Building docker image for SMB's load test"
cd $SCRIPT_DIR/../
./tools/scripts/versioninfo.sh
docker build -t "loadtest:latest" -f ./docker/ubuntu-focal-loadtest/Dockerfile .
