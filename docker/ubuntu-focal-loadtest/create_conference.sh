#!/bin/bash
set -e

if [ "$#" -ne 2 ]; then
    echo "Usage create_conference.sh <SMB IP> <SMB Port>. Eg. create_conference.sh 127.0.0.1 8080"
    exit
fi

echo "Attempting to create conference..."

rm -f conference_id.json
curl -X POST $1:$2/conferences -H 'Content-Type: application/json' -d '{"last-n":9,"global-port":true}' >> conference_id.json
export CONFERENCE_ID=`jq '.id' conference_id.json | head -n 1 | tr -d '"'`
