FROM centos:7

RUN yum -y update && yum -y upgrade && yum -y install epel-release
RUN yum -y install centos-release-scl
RUN yum -y install devtoolset-9

# Install build dependenices
RUN yum -y install git gtk-doc libtool lcov wget patchelf lsof
RUN yum -y --enablerepo=extras install epel-release
RUN yum -y install cmake3 ninja-build libstdc+
RUN ln -s cmake3 /usr/bin/cmake

RUN cd /tmp \
    && wget -O clang+llvm-8.0.1-x86_64-linux-gnu-ubuntu-14.04.tar.xz https://github.com/llvm/llvm-project/releases/download/llvmorg-8.0.1/clang+llvm-8.0.1-x86_64-linux-gnu-ubuntu-14.04.tar.xz \
    && tar xvf clang+llvm-8.0.1-x86_64-linux-gnu-ubuntu-14.04.tar.xz \
    && cd clang+llvm-8.0.1-x86_64-linux-gnu-ubuntu-* \
    && cp -rn * /usr/local \
    && cd /tmp && rm -rf clang+llvm-8.0.1-x86_64-linux-gnu-ubuntu-* \
    && ln -s /usr/local/bin/lld /usr/local/bin/ld

# Build libcxx to /opt/libcxx-8.0.1/libc++.so.1.0 with GLIBC-2.17
RUN cd /tmp \
    && wget -O libcxx-8.0.1.src.tar.xz https://github.com/llvm/llvm-project/releases/download/llvmorg-8.0.1/libcxx-8.0.1.src.tar.xz \
    && tar xvf libcxx-8.0.1.src.tar.xz \
    && cd libcxx-8.0.1.src \ 
    && source scl_source enable devtoolset-9 \
    && mkdir build \
    && cd build \
    && cmake ../ -DCMAKE_BUILD_TYPE=release -G "Unix Makefiles" . \
    && make -j$(getconf _NPROCESSORS_ONLN) \
    && cp -r lib/ /opt/libcxx-8.0.1

# Build and libcxx-abi to /tmp/libcxxabi-8.0.1.src/build/lib/libc++abi.so.1.0 with GLIBC-2.17
RUN cd /tmp \
    && wget -O libcxxabi-8.0.1.src.tar.xz https://github.com/llvm/llvm-project/releases/download/llvmorg-8.0.1/libcxxabi-8.0.1.src.tar.xz \
    && tar xvf libcxxabi-8.0.1.src.tar.xz \
    && cd libcxxabi-8.0.1.src \
    && source scl_source enable devtoolset-9 \
    && mkdir build \
    && cd build \
    && cmake ../ -DCMAKE_BUILD_TYPE=release -G "Unix Makefiles" . \
    && make -j$(getconf _NPROCESSORS_ONLN) \
    && unset LD_LIBRARY_PATH \
    && cp -r lib/ /opt/libcxxabi-8.0.1

# Build openssl-1.1.1s
RUN cd /tmp && wget https://www.openssl.org/source/openssl-1.1.1s.tar.gz && tar xvfz openssl-1.1.1s.tar.gz && rm openssl-1.1.1s.tar.gz
RUN cd /tmp/openssl-1.1.1s && ./config && make -j5 && make install_sw

# Build libsrtp 2.4.2
RUN cd /tmp && git clone https://github.com/cisco/libsrtp \
    && cd /tmp/libsrtp && git checkout v2.4.2 \
    && PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig ./configure --enable-openssl \
    && make -j5 \
    && make install

# Build libmicrohttpd 0.9.73
RUN cd /tmp && wget https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-0.9.73.tar.gz \
    && tar xvfz libmicrohttpd-0.9.73.tar.gz \
    && cd /tmp/libmicrohttpd-0.9.73 && ./configure --disable-https \
    && make -j5 \
    && make install

# Build opus 1.3.1
RUN cd /tmp && wget https://archive.mozilla.org/pub/opus/opus-1.3.1.tar.gz \
    && tar xvfz opus-1.3.1.tar.gz \
    && rm opus-1.3.1.tar.gz \
    && cd /tmp/opus-1.3.1 && ./configure \
    && make -j5 \
    && make install

# Restore old GCC's ld to build glibc
RUN ln -sf /usr/bin/ld /usr/local/bin/ld

RUN cd /tmp && wget http://ftp.gnu.org/gnu/glibc/glibc-2.18.tar.gz && tar zxvf glibc-2.18.tar.gz \
    && cd /tmp/glibc-2.18 && mkdir -p build && cd build \
    && CC=gcc CXX= LD_LIBRARY_PATH= ../configure --prefix=/opt/glibc-2.18 \
    && make -j$(getconf _NPROCESSORS_ONLN) \
    && LD_LIBRARY_PATH= make install

# Restore new CLANG's ld
RUN ln -sf /usr/local/bin/lld /usr/local/bin/ld

# Copy clang/c++ executable and libs to proper relative path in devtools root
RUN export PATH=/opt/rh/devtoolset-9/root/usr/local/bin:$PATH
RUN cp -r /usr/local/* /opt/rh/devtoolset-9/root/usr/
RUN cp -r /opt/rh/devtoolset-9/root/usr/lib/gcc/x86_64-redhat-linux/9/* /opt/rh/devtoolset-9/root/usr/local/bin/../lib64