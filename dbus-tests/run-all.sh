#!/bin/sh

while true; do
    ./test-getting-and-setting.sh
    ./test-leaks -s
    ./test-leaks -n
    ./test-schema-bug
    ./test-stale-value-bug
    sleep 5
done
