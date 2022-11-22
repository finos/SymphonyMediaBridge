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

if [ "$#" -ne 2 ]; then
    echo "Usage wait_for_others.sh <num instances> <timeout in seconds>. Eg. wait_for_others.sh 4 30"
    exit
fi

touch "$INSTANCE_ID.signal"
gsutil cp "$INSTANCE_ID.signal" "gs://$GCE_BUCKET_NAME"

echo "Waiting for total $1 participants for $2 seconds..."

expected=$(( $1 ))
participants_missing=$(( $1 ))
attempt=$(( $2 ))
while [ $(( participants_missing )) != 0 ] && [ $(( attempt )) != 0 ]
do
    x=$(( $(gsutil ls "gs://$GCE_BUCKET_NAME/*.signal" | wc -l ) ))
    participants_missing=$(( expected - x ))
    echo "Attempt $attempt... $x joined. $participants_missing missing..."
    attempt=$(( attempt - 1 ))
    sleep 1
done

if [ $participants_missing -eq 0 ]; then
    echo "Success. All participants signalled. Ready to continue the test."
    exit 0
else
    echo "Failure. $participants_missing participants still not joined."
    exit $participants_missing
fi