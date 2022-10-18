#!/bin/bash

set -e

SCRIPT_PATH="$(dirname "$0")"

FILE_TO_COPY=smb.barbell

ZONE_1=us-east4-b
ZONE_2=us-east4-c
ZONE_3=us-east4-a
INSTANCE=mbr-dev-99888-use4-h3l0
INSTANCE_2=mbr-dev-99888-use4-jtll
INSTANCE_3=mbr-dev-99888-use4-zdwv
PROJECT=sym-dev-mr-rtc-mbr
#PROJECT=sym-dev-rtc
INSTALL_PATH=/opt/rtc/mbr_v2/smb
#INSTALL_PATH=/opt/rtc/rtc-media-bridge/resources/el7/smb
IAP_FLAG=--tunnel-through-iap
#IAP_FLAG=


gcloud compute scp --zone "$ZONE_1" --project "$PROJECT" $IAP_FLAG -- \
    $INSTANCE:/var/log/rtc-media-bridge/smb.log smb.1.log

gcloud compute scp --zone "$ZONE_2" --project "$PROJECT" $IAP_FLAG -- \
    $INSTANCE_2:/var/log/rtc-media-bridge/smb.log smb.2.log

gcloud compute scp --zone "$ZONE_3" --project "$PROJECT" $IAP_FLAG -- \
    $INSTANCE_3:/var/log/rtc-media-bridge/smb.log smb.3.log

echo "DONE!"

#scp $SCRIPT_PATH/$FILE_TO_COPY st:/opt/rtc/mbr_v2/smb/$FILE_TO_COPY