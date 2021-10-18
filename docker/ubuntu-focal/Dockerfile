FROM ubuntu:focal

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get -y update && apt-get -y upgrade

# Install build dependenices
RUN apt-get -y install llvm git wget cmake libc++-dev libc++abi-dev clang libtool

# Build lcov 1.15
RUN cd /tmp && git clone https://github.com/linux-test-project/lcov.git
RUN cd /tmp/lcov && git checkout v1.15 && make install

# Build openssl-1.1.1l
RUN cd /tmp && wget https://www.openssl.org/source/openssl-1.1.1l.tar.gz && tar xvfz openssl-1.1.1l.tar.gz
RUN cd /tmp/openssl-1.1.1l && ./config && make && make install

# Build libsrtp 2.4.1
RUN cd /tmp && git clone https://github.com/cisco/libsrtp
RUN cd /tmp/libsrtp && git checkout v2.4.1 && PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig ./configure --enable-openssl && make && make install

# Build libmicrohttpd 0.9.73
RUN cd /tmp && wget https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-0.9.73.tar.gz && tar xvfz libmicrohttpd-0.9.73.tar.gz
RUN cd /tmp/libmicrohttpd-0.9.73 && ./configure --disable-https && make && make install

# Build opus 1.3.1
RUN cd /tmp && wget https://archive.mozilla.org/pub/opus/opus-1.3.1.tar.gz && tar xvfz opus-1.3.1.tar.gz
RUN cd /tmp/opus-1.3.1 && ./configure && make && make install
