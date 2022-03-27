FROM amazonlinux:2

RUN yum -y update && yum -y upgrade
RUN yum -y groupinstall "Development Tools"
RUN yum -y install git wget which
RUN amazon-linux-extras enable python3.8 && yum -y install python3.8

# Build openssl-1.1.1l
RUN cd /tmp \
    && wget https://www.openssl.org/source/openssl-1.1.1l.tar.gz && tar xvfz openssl-1.1.1l.tar.gz \
    && cd /tmp/openssl-1.1.1l \
    && ./config \
    && make -j8 \
    && make install


RUN cd /tmp && wget https://cmake.org/files/v3.18/cmake-3.18.0.tar.gz && tar -xvzf cmake-3.18.0.tar.gz \
    && cd cmake-3.18.0 && ./bootstrap && make && make install

RUN cd /tmp/ \
    && git clone https://github.com/llvm/llvm-project.git --branch release/9.x --single-branch --depth 1 \
    && cd /tmp/llvm-project/ \
    && cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_ENABLE_PROJECTS='clang;compiler-rt;lld;parallel-libs;polly;pstl;libcxx;libcxxabi' \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -S 'llvm' \
        -B 'build' \
    && cd build && make -j8 install \
    && cd /tmp/ && rm -rf llvm-project \
    && ln -s /usr/local/bin/lld /usr/local/bin/ld

# Build libsrtp 2.4.2
RUN cd /tmp \
    && git clone https://github.com/cisco/libsrtp \
    && cd /tmp/libsrtp \
    && git checkout v2.4.2 \
    && PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig ./configure --enable-openssl \
    && make -j8 \
    && make install

RUN cd /tmp \
    && wget https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-0.9.73.tar.gz \
    && tar xvzf libmicrohttpd-0.9.73.tar.gz \
    && cd /tmp/libmicrohttpd-0.9.73 \
    && ./configure --disable-https \
    && make -j8 \
    && make install

# Build opus 1.3.1
RUN cd /tmp \
    && wget https://archive.mozilla.org/pub/opus/opus-1.3.1.tar.gz \
    && tar xvfz opus-1.3.1.tar.gz \
    && cd /tmp/opus-1.3.1 \
    && ./configure \
    && make -j8 \
    && make install
