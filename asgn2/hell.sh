#!/bin/bash
for i in {1..300000}; do printf "PUT /foo.txt HTTP/1.1\r\nContent-Length: 300000\r\n\r\n" | nc -N localhost 1242; done