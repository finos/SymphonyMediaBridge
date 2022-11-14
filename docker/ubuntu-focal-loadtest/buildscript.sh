#!/bin/bash

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

VERSION=$2
PACKAGE_NAME=finos-rtc-smb

function pr() {
  echo -e ${GREEN}$1${NC}
}

mkdir -p ubuntu-focal-loadtest/smb
pushd ubuntu-focal-loadtest/smb

export CC=clang
export CXX=clang++

pr "Generating versioninfo"
../../tools/scripts/versioninfo.sh

pr "Generating make files"
../../docker/ubuntu-focal-loadtest/generate_makefile.sh $1
if [ $? != 0 ]; then
    pr "Could not generate make file."; exit 1
fi

pr "Building ..."

make -j5
if [ $? != 0 ]; then
    pr "Could not make."; exit 1
fi

mkdir -p ${PACKAGE_NAME}_${VERSION}/DEBIAN
mkdir -p ${PACKAGE_NAME}_${VERSION}/usr/bin
mkdir -p ${PACKAGE_NAME}_${VERSION}/usr/share/doc/${PACKAGE_NAME}
cp smb ${PACKAGE_NAME}_${VERSION}/usr/bin/.
cp ../../README.md ../../NOTICE ${PACKAGE_NAME}_${VERSION}/usr/share/doc/${PACKAGE_NAME}/.


cat > ${PACKAGE_NAME}_${VERSION}/DEBIAN/control << EOF
Package: ${PACKAGE_NAME}
Version: ${VERSION}
Section: base
Priority: optional
Architecture: amd64
Depends: libc++-dev (>= 1:10.0), libc++abi-dev (>= 1:10.0), libssl1.1 (>= 1.1.1f), libsrtp2-1 (>= 2.3.0-2), libmicrohttpd12 (>= 0.9.66-1), libopus0 (>= 1.3.1)
Maintainer: Symphony LLC
Description: Symphony Media Bridge
EOF


cat > ${PACKAGE_NAME}_${VERSION}/DEBIAN/postinst << EOF
#!/bin/bash
if [ ! -d "/etc/${PACKAGE_NAME}" ]; then
    mkdir /etc/${PACKAGE_NAME}
    echo "{}" > /etc/${PACKAGE_NAME}/config.json
fi
setcap CAP_SYS_NICE+ep /usr/bin/smb
EOF


chmod 755 ${PACKAGE_NAME}_${VERSION}/DEBIAN/postinst
dpkg-deb --build ${PACKAGE_NAME}_${VERSION}
rm -rf ${PACKAGE_NAME}_${VERSION}

mkdir ${PACKAGE_NAME}_${VERSION}
cp smb ../../README.md ../../NOTICE ${PACKAGE_NAME}_${VERSION}/.
echo "{}" > ${PACKAGE_NAME}_${VERSION}/config.json

tar cvf ${PACKAGE_NAME}_${VERSION}.tar ${PACKAGE_NAME}_${VERSION}
gzip ${PACKAGE_NAME}_${VERSION}.tar

popd
pr "Done building for Ubuntu Focal deb."
