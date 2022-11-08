#!/bin/bash
set -e

if [ -z "$INSTANCE_ID" ]
then
      echo "\$INSTANCE_ID env var should be set!"
      exit
fi

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

if [ "$#" -ne 4 ]; then
    echo "Usage startup.sh <initiator|guest> <SMB IP> <SMB Port> <num clients>. Eg. startup.sh initiator 127.0.0.1 8080"
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
                '{"ip": $ip, "port": '$3', "numClients": '$4', "conference_id":$conference_id'})
    echo $CONFIG_FILE > load_test_config.json
fi

$SCRIPT_DIR/../../LoadTest --gtest_also_run_disabled_tests --gtest_filter="RealTimeTest.DISABLED_smbMegaHoot" --load_test_config=load_test_config.json