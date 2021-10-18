#!/bin/bash

yum update
yum install -y nano unzip net-tools wget

unzip smb.zip
rm smb.zip

LOCAL_IP=`ifconfig eth1 |grep 'inet ' |awk '{print $2}'`
echo "Detected local ip: $LOCAL_IP"

chown -R vagrant:vagrant ./* 
