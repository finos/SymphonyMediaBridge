FROM amazonlinux:2

RUN yum -y update && yum -y upgrade
RUN yum -y groupinstall "Development Tools"
RUN yum -y install git wget which patchelf
RUN amazon-linux-extras enable python3.8 && yum -y install python3.8

# Build openssl-1.1.1s
RUN cd /tmp && wget https://www.openssl.org/source/openssl-1.1.1s.tar.gz && tar xvfz openssl-1.1.1s.tar.gz && rm openssl-1.1.1s.tar.gz
RUN cd /tmp/openssl-1.1.1s && ./config && make -j5 && make install_sw

RUN cd /tmp && wget https://cmake.org/files/v3.18/cmake-3.18.0.tar.gz && tar -xvzf cmake-3.18.0.tar.gz \
    && cd cmake-3.18.0 && ./bootstrap && make -j$(getconf _NPROCESSORS_ONLN) && make install

RUN cd /tmp \
    && wget -O clang+llvm-12.0.1-x86_64-linux.tar.xz https://github.com/llvm/llvm-project/releases/download/llvmorg-12.0.1/clang+llvm-12.0.1-x86_64-linux-gnu-ubuntu-16.04.tar.xz \
    && tar xvf clang+llvm-12.0.1-x86_64-linux.tar.xz \
    && cd clang+llvm-12.0.1-x86_64-linux-gnu-ubuntu-* \
    && cp -rn * /usr/local \
    && cd /tmp && rm -rf clang+llvm-12.0.1-x86_64-linux-gnu-ubuntu-* \
    && ln -s /usr/local/bin/lld /usr/local/bin/ld \
    && rm clang+llvm-12.0.1-x86_64-linux.tar.xz


# Build libsrtp 2.4.2
RUN cd /tmp \
    && git clone https://github.com/cisco/libsrtp \
    && cd /tmp/libsrtp \
    && git checkout v2.4.2 \
    && PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig ./configure --enable-openssl \
    && make -j5 \
    && make install

RUN cd /tmp \
    && wget https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-0.9.73.tar.gz \
    && tar xvzf libmicrohttpd-0.9.73.tar.gz \
    && cd /tmp/libmicrohttpd-0.9.73 \
    && ./configure --disable-https \
    && make -j5 \
    && make install

# Build opus 1.3.1
RUN cd /tmp \
    && wget https://archive.mozilla.org/pub/opus/opus-1.3.1.tar.gz \
    && tar xvfz opus-1.3.1.tar.gz \
    && cd /tmp/opus-1.3.1 \
    && ./configure \
    && make -j5 \
    && make install

# Restore old GCC's ld to build glibc
RUN ln -sf /usr/bin/ld /usr/local/bin/ld

RUN wget -qO- http://ftp.gnu.org/gnu/glibc/glibc-2.18.tar.gz | tar zxv -C /tmp/ \
    && cd /tmp/glibc-2.18 && mkdir -p build && cd build \
    && CC=gcc CXX= LD_LIBRARY_PATH= ../configure --prefix=/opt/glibc-2.18 \
    && make -j$(getconf _NPROCESSORS_ONLN) \
    && LD_LIBRARY_PATH= make install \
    && cd /tmp && rm -rf /tmp/glibc-2.18

# Restore new CLANG's ld
RUN ln -sf /usr/local/bin/lld /usr/local/bin/ld


# Install Node 20 for running Github Actions with glibc-2.28
# to avoid mouting a node version that does not support current GLIBC installed
RUN wget -qO- https://nodejs.org/dist/v20.17.0/node-v20.17.0.tar.gz | tar zxv -C /tmp/ \
    && cd /tmp/node-v20.17.0 \
    && CC=clang CXX=clang++ CXXFLAGS="-stdlib=libc++" LDFLAGS="-L/usr/local/lib/ -Wl,-rpath,/usr/local/lib/ -lc++ -lc++abi" LD_LIBRARY_PATH=/opt/glibc-2.28/lib PATH=/opt/glibc-2.28/bin:$PATH ./configure --prefix=/opt/node-20.17 \
    && CC=clang CXX=clang++ CXXFLAGS="-stdlib=libc++" LDFLAGS="-L/usr/local/lib/ -Wl,-rpath,/usr/local/lib/ -lc++ -lc++abi" LD_LIBRARY_PATH=/opt/glibc-2.28/lib PATH=/opt/glibc-2.28/bin:$PATH make -j$(nproc) \
    && CC=clang CXX=clang++ CXXFLAGS="-stdlib=libc++" LDFLAGS="-L/usr/local/lib/ -Wl,-rpath,/usr/local/lib/ -lc++ -lc++abi" LD_LIBRARY_PATH=/opt/glibc-2.28/lib PATH=/opt/glibc-2.28/bin:$PATH make -j$(nproc) install \
    && cd /tmp && rm -rf /tmp/node-v20.17.0

ENV PATH="$PATH:/opt/node-20.17/bin"

