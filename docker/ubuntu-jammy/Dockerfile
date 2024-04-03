FROM ubuntu:jammy

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get -y update --fix-missing && apt-get -y upgrade

# Install build dependenices
RUN apt-get -y install git wget cmake xz-utils libz-dev build-essential

# Build openssl-1.1.1s
RUN cd /tmp && wget https://www.openssl.org/source/openssl-1.1.1s.tar.gz && tar xvfz openssl-1.1.1s.tar.gz && rm openssl-1.1.1s.tar.gz
RUN cd /tmp/openssl-1.1.1s && ./config && make -j5 && make install_sw

# Build libsrtp 2.4.2
RUN cd /tmp && git clone https://github.com/cisco/libsrtp
RUN cd /tmp/libsrtp && git checkout v2.4.2 && PKG_CONFIG_PATH=/usr/local/lib/pkgconfig ./configure --enable-openssl && make -j5 && make install

RUN cd /tmp \
    && wget -O clang+llvm-12.0.1-x86_64-linux.tar.xz https://github.com/llvm/llvm-project/releases/download/llvmorg-12.0.1/clang+llvm-12.0.1-x86_64-linux-gnu-ubuntu-16.04.tar.xz \
    && tar xvf clang+llvm-12.0.1-x86_64-linux.tar.xz \
    && cd clang+llvm-12.0.1-x86_64-linux-gnu-ubuntu-* \
    && cp -rn * /usr/local \
    && cd /tmp && rm -rf clang+llvm-12.0.1-x86_64-linux-gnu-ubuntu-* \
    && ln -s /usr/local/bin/lld /usr/local/bin/ld \
    && rm clang+llvm-12.0.1-x86_64-linux.tar.xz

# Build lcov 1.15
RUN cd /tmp && git clone https://github.com/linux-test-project/lcov.git
RUN cd /tmp/lcov && git checkout v1.15 && make install

# Build libmicrohttpd 0.9.73
RUN cd /tmp && wget https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-0.9.73.tar.gz && tar xvfz libmicrohttpd-0.9.73.tar.gz && rm libmicrohttpd-0.9.73.tar.gz
RUN cd /tmp/libmicrohttpd-0.9.73 && ./configure --disable-https && make -j5 && make install

# Build opus 1.3.1
RUN cd /tmp && wget https://archive.mozilla.org/pub/opus/opus-1.3.1.tar.gz && tar xvfz opus-1.3.1.tar.gz
RUN cd /tmp/opus-1.3.1 && ./configure && make -j5 && make install
