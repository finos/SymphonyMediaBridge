FROM redhat/ubi8:latest

RUN yum -y update && yum -y upgrade

# Install build dependenices
RUN yum -y install git llvm-toolset cmake libtool wget lsof python3 make perl-Pod-Html xz

# Build lcov 1.15
RUN cd /tmp && git clone https://github.com/linux-test-project/lcov.git
RUN cd /tmp/lcov && git checkout v1.15 && make install

# Fetch llvm and build libc++ 10
RUN cd /tmp/ && git clone https://github.com/llvm/llvm-project.git --branch release/10.x --single-branch --depth 1
RUN cd /tmp/llvm-project/ && mkdir build
RUN cd /tmp/llvm-project/build && cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS='libcxx;libcxxabi' ../llvm/
RUN cd /tmp/llvm-project/build && make -j5 cxx && make install-cxx install-cxxabi

# Build openssl-1.1.1l
RUN cd /tmp && wget https://www.openssl.org/source/openssl-1.1.1l.tar.gz && tar xvfz openssl-1.1.1l.tar.gz
RUN cd /tmp/openssl-1.1.1l && ./config && make && make install

# Build libsrtp 2.4.2
RUN cd /tmp && git clone https://github.com/cisco/libsrtp
RUN cd /tmp/libsrtp && git checkout v2.4.2 && PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig ./configure --enable-openssl && make && make install

# Build libmicrohttpd 0.9.73
RUN cd /tmp && wget https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-0.9.73.tar.gz && tar xvfz libmicrohttpd-0.9.73.tar.gz
RUN cd /tmp/libmicrohttpd-0.9.73 && ./configure --disable-https && make && make install

# Build opus 1.3.1
RUN cd /tmp && wget https://archive.mozilla.org/pub/opus/opus-1.3.1.tar.gz && tar xvfz opus-1.3.1.tar.gz
RUN cd /tmp/opus-1.3.1 && ./configure && make && make install
