#/bin/sh

set -xe

clang++ -g src/client/main.cpp -o build/client.exe -I thirdparty -I thirdparty/ogg/include -I thirdparty/opus/include -L thirdparty -lopusfile