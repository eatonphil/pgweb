#!/bin/bash

for ((i=0;i<10000;i++)); do
	curl "localhost:9003/hello?name=Test$i"
done

#curl 'localhost:9003/_exit'
