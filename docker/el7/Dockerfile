FROM centos:7

RUN yum -y update && yum -y upgrade && yum -y install epel-release
RUN yum -y install centos-release-scl

# Install build dependenices
RUN yum -y install git devtoolset-7 llvm-toolset-7 gtk-doc libtool lcov wget patchelf lsof
RUN yum -y --enablerepo=extras install epel-release
RUN yum -y install cmake3
RUN ln -s cmake3 /usr/bin/cmake

# Fetch llvm and build libc++
RUN cd /tmp/ && git clone https://github.com/llvm/llvm-project.git --branch release/9.x --single-branch --depth 1 \
    && cd /tmp/llvm-project/ && mkdir build && cd build \
    && source scl_source enable llvm-toolset-7 \
    && cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS='libcxx;libcxxabi' ../llvm/ \
    && make -j5 cxx \
    && make install-cxx install-cxxabi

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

RUN cd /tmp && wget http://ftp.gnu.org/gnu/glibc/glibc-2.18.tar.gz && tar zxvf glibc-2.18.tar.gz \
    && cd /tmp/glibc-2.18 && mkdir -p build && cd build \
    && CC= CXX= LD_LIBRARY_PATH= ../configure --prefix=/opt/glibc-2.18 \
    && make -j$(getconf _NPROCESSORS_ONLN) \
    && LD_LIBRARY_PATH= make install
