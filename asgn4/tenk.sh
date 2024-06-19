#!/usr/bin/env bash

port="$1"

for i in {1..1000}
do
    echo -e -n "GET /bible.txt HTTP/1.1\r\n\r\n" | nc -N -C localhost $port > /dev/null &
done