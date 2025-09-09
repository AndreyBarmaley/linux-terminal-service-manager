#!/bin/bash
#
# $1 = digest:sha256:xxxxxxxx
#

if [ "$1" == "digest:sha256:00112233445566778899aabbccddeeff" ]; then
    echo "nobody"
    exit 0
fi

echo "invalid"
