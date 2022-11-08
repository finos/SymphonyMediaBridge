FROM ubuntu:focal

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get -y update

RUN apt-get -y install llvm git wget cmake libc++-dev libc++abi-dev clang libtool lcov libssl-dev libsrtp2-dev libmicrohttpd-dev libopus-dev zip curl jq

ADD ["./", "/build"]

WORKDIR /build

RUN rm -rf CMakeCache.txt

RUN ./docker/ubuntu-focal-loadtest/buildscript.sh Release

ENTRYPOINT [ "./docker/ubuntu-focal-loadtest/startup.sh" ]