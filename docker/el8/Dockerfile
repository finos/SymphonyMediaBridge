FROM redhat/ubi8:latest

RUN yum -y update && yum -y upgrade

# Install build dependencies
RUN yum -y install git cmake llvm-toolset lld libtool wget lsof python3 perl-IO-Compress perl-JSON-PP perl-IPC-Cmd make perl-Pod-Html xz

# Build lcov 1.15
RUN cd /tmp && git clone https://github.com/linux-test-project/lcov.git
RUN cd /tmp/lcov && git checkout v1.15 && make install

# Fetch llvm and build libc++ 10
RUN cd /tmp/ && git clone https://github.com/llvm/llvm-project.git --branch release/10.x --single-branch --depth 1
RUN cd /tmp/llvm-project/ && mkdir build
RUN cd /tmp/llvm-project/build && cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS='libcxx;libcxxabi' ../llvm/
RUN cd /tmp/llvm-project/build && make -j5 cxx && make install-cxx install-cxxabi

# Build openssl-3.4.0
RUN cd /tmp && wget https://github.com/openssl/openssl/releases/download/openssl-3.4.0/openssl-3.4.0.tar.gz && tar xvfz openssl-3.4.0.tar.gz && rm openssl-3.4.0.tar.gz
RUN cd /tmp/openssl-3.4.0 && ./config && make -j5 && make install_sw

RUN yum -y remove cmake && cd /tmp && wget https://cmake.org/files/v3.30/cmake-3.30.0.tar.gz && tar -xvzf cmake-3.30.0.tar.gz \
    && cd cmake-3.30.0 && ./bootstrap && make -j5 && make install

# Build libsrtp 2.6.0
RUN cd /tmp \
    && git clone https://github.com/cisco/libsrtp \
    && cd /tmp/libsrtp \
    && git checkout v2.6.0 \
    && PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig ./configure --enable-openssl \
    && make -j5 \
    && make install

# Build libmicrohttpd 0.9.73
RUN cd /tmp && wget https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-0.9.73.tar.gz && tar xvfz libmicrohttpd-0.9.73.tar.gz
RUN cd /tmp/libmicrohttpd-0.9.73 && ./configure --disable-https && make -j5 && make install

# Build opus 1.3.1
RUN cd /tmp && wget https://archive.mozilla.org/pub/opus/opus-1.3.1.tar.gz && tar xvfz opus-1.3.1.tar.gz
RUN cd /tmp/opus-1.3.1 && ./configure && make -j5 && make install
