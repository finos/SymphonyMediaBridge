#!/bin/bash
set -e

if [ -z "$INSTANCE_ID" ]
then
      echo "\$INSTANCE_ID env var should be set!"
      exit
fi

if [ -z "$GCE_BUCKET_NAME" ]
then
      echo "\$GCE_BUCKET_NAME env var should be set!"
      exit
fi

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

if [ "$#" -ne 6 ]; then
    echo "Usage startup.sh <initiator|guest> <SMB IP> <SMB Port> <num clients per instance> <num instances> <gtest filter>. Eg. for 1000 clients: startup.sh initiator 127.0.0.1 8080 250 4 RealTimeTest.DISABLED_smbMegaHoot"
    exit
fi

if [ "$1" != "initiator" ] && [ "$1" != "guest" ]; then
    echo "First param should be <initiator|guest>"
    exit
fi

echo "Statring with SMB IP: $2:$3, $1, Instance ID = $INSTANCE_ID, number of test instances = $5, each with $4 clients. gtest-filter = $6"

if [ "$1" == "initiator" ]; then

    # Create conference and obtain CONFERENCE_ID.
    source $SCRIPT_DIR/create_conference.sh $2 $3

    if [ -z "$CONFERENCE_ID" ]
    then
      echo "create_conference.sh failed! Exiting..."
      exit
    fi

    # Create and share config file that contains CONFERENCE_ID.
    CONFIG_FILE=$(jq --null-input \
                --arg ip "$2" \
                --arg conference_id "$CONFERENCE_ID" \
                '{"ip": $ip, "port": '$3', "numClients": '$4', "conference_id":$conference_id, "initiator":true}')
    echo $CONFIG_FILE > load_test_config.json
    jq '.initiator = false' load_test_config.json > load_test_config_for_guests.json
    gsutil cp load_test_config_for_guests.json gs://$GCE_BUCKET_NAME/load_test_config.json
else
    # If not initiator, retreive load_test_config.json.
    attempt=$(( 20 ))
    while [ $(( attempt )) != 0 ]
        do
        gsutil cp "gs://$GCE_BUCKET_NAME/load_test_config.json" ./
        if [ $? -eq 0 ]; then
            break
        fi
        attempt=$(( attempt - 1 ))
        sleep 1
    done
fi

# Wait for other test instances.
if ! $SCRIPT_DIR/wait_for_others.sh $5 30; then
    echo "Wait for others failed, $? participants are missing. Exiting..."
    exit
fi

# Execute tests.
$SCRIPT_DIR/../../LoadTest --gtest_also_run_disabled_tests --gtest_filter="RealTimeTest.DISABLED_smbMegaHoot" --load_test_config="$6"

# Upload results.
gsutil cp smb_load_test.log "gs://$GCE_BUCKET_NAME/$INSTANCE_ID/smb_load_test.log"

# Cleanup. (TODO: use "if $AUTO_DELETE ; then")
gcloud compute instances delete -q $INSTANCE_ID --zone $GCE_ZONE
