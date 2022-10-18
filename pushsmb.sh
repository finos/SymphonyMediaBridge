#!/bin/bash

# gcloud compute scp ..\rtc-media-bridge\target\rtc-media-bridge-21.1.0-SNAPSHOT.jar  %1:/home/olof_kallander_symphony_com/ --tunnel-through-iap --zone=us-east4-c --project="sym-dev-mr-rtc-mbr" 
gcloud compute scp smb  $1:/home/olof_kallander_symphony_com/ --tunnel-through-iap --zone=us-east4-c --project="sym-dev-mr-rtc-mbr"
gcloud compute scp prepsmb.sh  $1:/home/olof_kallander_symphony_com/ --tunnel-through-iap --zone=us-east4-c --project="sym-dev-mr-rtc-mbr"
#gcloud compute scp ubuntu-focal/smb/libs/*  $1:/home/olof_kallander_symphony_com/libs --tunnel-through-iap --zone=us-east4-c --project="sym-dev-mr-rtc-mbr"
