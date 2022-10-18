#!/bin/bash
chmod +x smb
mv smb /opt/rtc/mbr_v2/smb/
mv rtc-media-bridge*.jar /opt/rtc/mbr_v2/target/rtc-media-bridge-master-966.jar
cp libs/* /opt/rtc/mbr_v2/smb/libs
service rtc-media-bridge restart

