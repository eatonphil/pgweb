#!/bin/bash

for ((i=0;i<10000;i++)); do
	curl "127.0.0.1:9003/hello?name=Test$i"
done
