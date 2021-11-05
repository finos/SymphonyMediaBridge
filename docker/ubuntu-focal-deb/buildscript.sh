#!/bin/bash

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

VERSION=$2

function pr() {
  echo -e ${GREEN}$1${NC}
}

mkdir -p ubuntu-focal-deb/smb
pushd ubuntu-focal-deb/smb

export CC=clang
export CXX=clang++

pr "Generating versioninfo"
../../tools/scripts/versioninfo.sh

pr "Generating make files"
../../docker/ubuntu-focal-deb/generate_makefile.sh $1
if [ $? != 0 ]; then
    pr "Could not generate make file."; exit 1
fi

pr "Building ..."

make -j5
if [ $? != 0 ]; then
    pr "Could not make."; exit 1
fi

mkdir -p symphonymediabridge_${VERSION}/DEBIAN
mkdir -p symphonymediabridge_${VERSION}/usr/bin
mkdir -p symphonymediabridge_${VERSION}/usr/share/doc/symphonymediabridge
cp smb symphonymediabridge_${VERSION}/usr/bin/.
cp ../../README.md ../../NOTICE symphonymediabridge_${VERSION}/usr/share/doc/symphonymediabridge/.


cat > symphonymediabridge_${VERSION}/DEBIAN/control << EOF
Package: symphonymediabridge
Version: ${VERSION}
Section: base
Priority: optional
Architecture: amd64
Depends: libc++-dev (>= 1:10.0), libc++abi-dev (>= 1:10.0), libssl1.1 (>= 1.1.1f), libsrtp2-1 (>= 2.3.0-2), libmicrohttpd12 (>= 0.9.66-1), libopus0 (>= 1.3.1)
Maintainer: Symphony LLC
Description: Symphony Media Bridge
EOF


cat > symphonymediabridge_${VERSION}/DEBIAN/postinst << EOF
#!/bin/bash
if [ ! -d "/etc/symphonymediabridge" ]; then
    mkdir /etc/symphonymediabridge
    echo "{}" > /etc/symphonymediabridge/config.json
fi
setcap CAP_SYS_NICE+ep /usr/bin/smb
EOF


chmod 755 symphonymediabridge_${VERSION}/DEBIAN/postinst
dpkg-deb --build symphonymediabridge_${VERSION}
rm -rf symphonymediabridge_${VERSION}

mkdir symphonymediabridge_${VERSION}
cp smb ../../README.md ../../NOTICE symphonymediabridge_${VERSION}/.
echo "{}" > symphonymediabridge_${VERSION}/config.json

tar cvf symphonymediabridge_${VERSION}.tar symphonymediabridge_${VERSION}
gzip symphonymediabridge_${VERSION}.tar

popd
pr "Done building for Ubuntu Focal deb."
