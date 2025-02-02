#/bin/sh

set -xe

gcc src/server/main.c -o build/server ./thirdparty/coroutines/coroutine.c -I ./thirdparty/coroutines -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast