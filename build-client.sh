#/bin/sh

set -xe

clang src/client/main.c -o build/client.exe -I thirdparty