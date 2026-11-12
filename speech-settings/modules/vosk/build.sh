#!/bin/sh
# compiles vosk-test and sd_vosk in the sd-vosk-build image
set -e
cd "$(dirname "$0")"
docker run --rm -u $(id -u):$(id -g) -v $PWD:/w -w /w sd-vosk-build sh -c '
F="-O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-result $(pkg-config --cflags glib-2.0 json-glib-1.0)"
L="$(pkg-config --libs glib-2.0 json-glib-1.0 libonnxruntime) -lm"
gcc $F vosk.c vosk-test.c -o vosk-test $L
gcc $F -I/usr/include/speech-dispatcher -Ishim vosk.c sd_vosk.c -o sd_vosk -lspeechd_module $L -lpthread'
