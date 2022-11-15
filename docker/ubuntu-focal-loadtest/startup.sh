#!/bin/bash
set -e

function cleanup {
if [ -n "$log_file" ] && [ -f "$log_file" ] ; then
    gsutil cp "$log_file" "gs://$GCE_BUCKET_NAME"
    gcloud compute instances delete -q "$INSTANCE_ID" --zone "$GCE_ZONE"
fi
}

trap cleanup EXIT

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

if [ "$#" -ne 7 ]; then
    echo "Usage startup.sh <initiator|guest> <SMB IP> <SMB Port> <num clients per instance> <num instances> <gtest filter> <duration>. Eg. for 1000 clients: startup.sh initiator 127.0.0.1 8080 250 4 RealTimeTest.DISABLED_smbMegaHoot 30"
    exit
fi

if [ "$1" != "initiator" ] && [ "$1" != "guest" ]; then
    echo "First param should be <initiator|guest>"
    exit
fi

log_file="$INSTANCE_ID-test-output.log"

echo "Statring with SMB IP: $2:$3, $1, Instance ID = $INSTANCE_ID, number of test instances = $5, each with $4 clients. gtest-filter = $6, duration = $7 seconds" 2>&1 | tee $log_file

if [ "$1" == "initiator" ]; then

    # Create conference and obtain CONFERENCE_ID.
    source $SCRIPT_DIR/create_conference.sh $2 $3 2>&1 | tee -a $log_file

    if [ -f "conference_id.json" ]; then
        CONFERENCE_ID=$(jq '.id' conference_id.json | head -n 1 | tr -d '"')
        echo "Conference ID: $CONFERENCE_ID" 2>&1 | tee -a $log_file
    else
        exit
    fi

    if [ -z "${CONFERENCE_ID}" ]
    then
      echo "create_conference.sh failed! Exiting..." 2>&1 | tee -a $log_file
      exit
    fi

    # Create and share config file that contains CONFERENCE_ID.
    CONFIG_FILE=$(jq --null-input \
                --arg ip "$2" \
                --arg conference_id "$CONFERENCE_ID" \
                '{"ip": $ip, "port": '$3', "numClients": '$4', "conference_id":$conference_id, "initiator":true, "duration": '$7'}')
    echo $CONFIG_FILE > load_test_config.json
    jq '.initiator = false' load_test_config.json > load_test_config_for_guests.json
    gsutil cp load_test_config_for_guests.json "gs://$GCE_BUCKET_NAME/load_test_config.json" 2>&1 | tee -a $log_file
else
    # If not initiator, retreive load_test_config.json.
    attempt=$(( 20 ))
    while [ $(( attempt )) != 0 ]
        do
        gsutil cp "gs://$GCE_BUCKET_NAME/load_test_config.json" ./ 2>&1 | tee -a $log_file
        if [ $? -eq 0 ]; then
            break
        fi
        attempt=$(( attempt - 1 ))
        sleep 1
    done
fi

if [ ! -f load_test_config.json ]; then
    echo "Failed to receive config file load_test_config.json. Exiting..."  2>&1 | tee -a $log_file
    exit
fi

# Wait for other test instances.
source $SCRIPT_DIR/wait_for_others.sh "$5" 30 2>&1 | tee -a $log_file
if [ ! "$?" ]; then
    echo "Wait for others failed, $? participants are missing. Exiting..."  2>&1 | tee -a $log_file
    exit
fi

echo "Starting tests..." | tee -a $log_file

execFileName=$(find -type f -name "LoadTest")
execFileName=$(realpath "$execFileName")
loadTestConfig=$(realpath load_test_config.json)

echo "Local config:" | tee -a $log_file
cat $loadTestConfig 2>&1 | tee -a $log_file

test_cmd='$execFileName --gtest_also_run_disabled_tests --gtest_filter=$6 --gtest_output=xml:results.xml --load_test_config=$loadTestConfig'
echo "Test cmd line: $test_cmd" 2>&1 | tee -a $log_file

# Execute tests.
eval "$test_cmd" 2>&1 | tee -a $log_file

# Upload results.
gsutil cp smb_load_test.log "gs://$GCE_BUCKET_NAME/$INSTANCE_ID/smb_load_test.log" 2>&1 | tee -a $log_file
gsutil cp results.xml "gs://$GCE_BUCKET_NAME/$INSTANCE_ID/results.xml" 2>&1 | tee -a $log_file