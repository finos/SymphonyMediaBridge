#!/bin/bash
set -e

if [ -z "$INSTANCE_ID" ]
then
      echo "\$INSTANCE_ID env var should be set!"
      exit
fi

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

if [ "$#" -ne 5 ]; then
    echo "Usage startup.sh <initiator|guest> <SMB IP> <SMB Port> <num clients per instance> <num instances>. Eg. for 1000 clients: startup.sh initiator 127.0.0.1 8080 250 4"
    exit
fi

if [ "$1" != "initiator" ] && [ "$1" != "guest" ]; then
    echo "First param should be <initiator|guest>"
    exit
fi

echo "Statring with SMB IP: $2:$3, $1, Instance ID = $INSTANCE_ID"

if [ "$1" == "initiator" ]; then
    source $SCRIPT_DIR/create_conference.sh $2 $3
    CONFIG_FILE=$(jq --null-input \
                --arg ip "$2" \
                --arg conference_id "$CONFERENCE_ID" \
                '{"ip": $ip, "port": '$3', "numClients": '$4', "conference_id":$conference_id, "initiator":true}')
    echo $CONFIG_FILE > load_test_config.json
    jq '.initiator = false' load_test_config.json > load_test_config_for_guests.json
    #TODO 1: upload "load_test_config_for_guests.json" to GCP bucket
else
    #TODO 2: wait for "load_test_config_for_guests.json" to the GCP bucket and download it as "load_test_config.json"
fi

    #TODO 3: upload $INSTANCE_ID.signal to the GCP bucket
    #TODO 4: wait for $5 (<num instances>) *.signal files in the GCP bucket


$SCRIPT_DIR/../../LoadTest --gtest_also_run_disabled_tests --gtest_filter="RealTimeTest.DISABLED_smbMegaHoot" --load_test_config=load_test_config.json

    #TODO 5: upload log files to the GCP bucket