FROM centos:7

RUN yum -y update && yum -y install epel-release
RUN yum -y install centos-release-scl

# Install build dependenices
RUN yum -y install git devtoolset-7 llvm-toolset-7 llvm-toolset-7-cmake gtk-doc libtool lcov wget patchelf lsof

# Fetch llvm and build libc++
RUN cd /tmp/ && git clone https://github.com/llvm/llvm-project.git --branch release/9.x --single-branch --depth 1
RUN cd /tmp/llvm-project/ && mkdir build
RUN cd /tmp/llvm-project/build && source scl_source enable llvm-toolset-7 && cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS='libcxx;libcxxabi' ../llvm/
RUN cd /tmp/llvm-project/build && source scl_source enable llvm-toolset-7 && make -j5 cxx && make install-cxx install-cxxabi

# Build openssl-1.1.1h
RUN cd /tmp && wget https://www.openssl.org/source/openssl-1.1.1h.tar.gz && tar xvfz openssl-1.1.1h.tar.gz
RUN cd /tmp/openssl-1.1.1h && ./config && make && make install

# Build libsrtp 2.3.0
RUN cd /tmp && git clone https://github.com/cisco/libsrtp
RUN cd /tmp/libsrtp && git checkout v2.3.0 && PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig ./configure --enable-openssl && make && make install

# Build libmicrohttpd 0.9.71
RUN cd /tmp && wget https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-0.9.71.tar.gz && tar xvfz libmicrohttpd-0.9.71.tar.gz
RUN cd /tmp/libmicrohttpd-0.9.71 && ./configure --disable-https && make && make install

# Build opus 1.3.1
RUN cd /tmp && wget https://archive.mozilla.org/pub/opus/opus-1.3.1.tar.gz && tar xvfz opus-1.3.1.tar.gz
RUN cd /tmp/opus-1.3.1 && ./configure && make && make install


RUN cd /tmp && wget http://ftp.gnu.org/gnu/glibc/glibc-2.18.tar.gz && tar zxvf glibc-2.18.tar.gz
RUN cd /tmp/glibc-2.18 && mkdir -p build && cd build && CC= CXX= LD_LIBRARY_PATH= ../configure --prefix=/opt/glibc-2.18 && make -j$(getconf _NPROCESSORS_ONLN) && LD_LIBRARY_PATH= make install
