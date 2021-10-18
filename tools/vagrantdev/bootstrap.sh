#!/bin/bash

yum update
yum-config-manager --enable rhel-server-rhscl-7-rpms
yum install -y nano unzip net-tools wget centos-release-scl devtoolset-7 llvm-toolset-7 

#unzip smb.zip
#rm smb.zip

LOCAL_IP=`ifconfig eth1 |grep 'inet ' |awk '{print $2}'`
echo "Detected local ip: $LOCAL_IP"

chown -R vagrant:vagrant ./* 

source scl_source enable devtoolset-7

