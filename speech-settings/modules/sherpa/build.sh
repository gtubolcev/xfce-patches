#!/bin/sh
# compiles sd_sherpa in the sd-build image against the extracted sherpa-onnx package (sx/)
set -e
cd "$(dirname "$0")"
docker run --rm -u $(id -u):$(id -g) -v $PWD:/w -w /w sd-build sh -c 'gcc -O2 -Wall -Wextra -Wno-unused-parameter -I/usr/include/speech-dispatcher -Ishim -Isx/usr/include $(pkg-config --cflags glib-2.0) sd_sherpa.c -o sd_sherpa -lspeechd_module -Lsx/usr/lib/x86_64-linux-gnu -lsherpa-onnx-c-api -Wl,--allow-shlib-undefined $(pkg-config --libs glib-2.0) -lpthread -lm'
