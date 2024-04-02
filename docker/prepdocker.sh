#!/bin/bash
if [ "$#" -ne 1 ]; then
    echo "Usage prepdocker.sh <el7|el8|ubuntu-focal|ubuntu-jammy|aws-linux>. Eg. prepdocker.sh el8"
    exit
fi

echo "Building docker image for smb" $1
cd $1
../../tools/scripts/versioninfo.sh
docker build -t "buildsmb-$1:latest" .
