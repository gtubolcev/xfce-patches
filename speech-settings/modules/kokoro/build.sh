#!/bin/sh
# compiles kokoro-test in the sd-kokoro-build image
set -e
cd "$(dirname "$0")"
docker run --rm -u $(id -u):$(id -g) -v $PWD:/w -w /w sd-kokoro-build sh -c '
F="-O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-result $(pkg-config --cflags glib-2.0 json-glib-1.0)"
L="$(pkg-config --libs glib-2.0 json-glib-1.0 libonnxruntime) -lespeak-ng -lm"
gcc $F ru-g2p.c kokoro.c kokoro-test.c -o kokoro-test $L'
docker run --rm -u $(id -u):$(id -g) -v $PWD:/w -w /w sd-kokoro-build sh -c '
F="-O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-result $(pkg-config --cflags glib-2.0 json-glib-1.0)"
L="$(pkg-config --libs glib-2.0 json-glib-1.0 libonnxruntime) -lespeak-ng -lm"
gcc $F -I/usr/include/speech-dispatcher -Ishim ru-g2p.c kokoro.c sd_kokoro.c -o sd_kokoro -lspeechd_module $L -lpthread'
