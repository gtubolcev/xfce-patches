#!/bin/sh
# Builds speech-dispatcher-vosk (sd_vosk + default config) and
# vosk-tts-model-ru-0.9-multi (the model with a presorted dictionary) here.
# MODEL: the unpacked vosk-model-tts-ru-0.9-multi from alphacephei.com.
set -e
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
MODEL=${MODEL:-/home/grigory/Storage/Claude/vosk/model/vosk-model-tts-ru-0.9-multi}
OUT=${OUT:-/home/grigory/Storage/Claude/vosk/deb}
VER=0.1-3~local
MVER=0.9-1~local
rm -rf "$OUT/root"; mkdir -p "$OUT/root"

# ------------------------------------------------------------ the module
W=$OUT/root/speech-dispatcher-vosk
D=$W/usr/share/doc/speech-dispatcher-vosk
mkdir -p $W/DEBIAN $W/usr/lib/speech-dispatcher-modules $W/etc/speech-dispatcher/modules $D
$HERE/build.sh
install -m755 $HERE/sd_vosk $W/usr/lib/speech-dispatcher-modules/sd_vosk
strip $W/usr/lib/speech-dispatcher-modules/sd_vosk
install -m644 $HERE/vosk.conf $W/etc/speech-dispatcher/modules/vosk.conf
echo /etc/speech-dispatcher/modules/vosk.conf > $W/DEBIAN/conffiles
cat > $D/copyright <<'X'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: sd_vosk

Files: *
Copyright: 2026 Grigory Tuboltsev
License: MIT
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 .
 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.
 .
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
Comment: the text processing is a port of the vosk-tts Python package
 (Apache-2.0, Alpha Cephei Inc.). Statically linked with libspeechd_module
 from speech-dispatcher (LGPL-2.1+), which is allowed for the object code;
 the module source is in this directory (*.c.gz, vosk.h).
X
for f in sd_vosk.c vosk.c; do gzip -9n -c $HERE/$f > $D/$f.gz; done
install -m644 $HERE/vosk.h $D/vosk.h
cat > $W/DEBIAN/control <<X
Package: speech-dispatcher-vosk
Version: $VER
Architecture: amd64
Maintainer: Grigory Tuboltsev <gtubolcev@gmail.com>
Section: sound
Priority: optional
Depends: libc6, libglib2.0-0t64, libjson-glib-1.0-0, libonnxruntime1.23,
 speech-dispatcher (>= 0.12), vosk-tts-model-ru-0.9-multi
Installed-Size: $(du -sk --total $W/usr $W/etc | tail -1 | cut -f1)
Description: Speech Dispatcher module for Vosk TTS voices
 Speaks with the neural voices of Vosk TTS (Alpha Cephei) through ONNX
 Runtime, without Python: the Russian multi-speaker model has 5 voices
 (3 female, 2 male) with stress marks from a large dictionary and the
 sentence intonation from a BERT model. The model is loaded into the
 module once and stays loaded.
 Settings in /etc/speech-dispatcher/modules/vosk.conf.
X
dpkg-deb --root-owner-group -b $W $OUT/speech-dispatcher-vosk_${VER}_amd64.deb

[ -n "$MODULE_ONLY" ] && exit 0

# ------------------------------------------------------------- the model
N=vosk-model-tts-ru-0.9-multi
W=$OUT/root/$N
P=$W/usr/share/vosk-tts/$N
D=$W/usr/share/doc/vosk-tts-model-ru-0.9-multi
mkdir -p $W/DEBIAN $P/bert $D
install -m644 $MODEL/config.json $MODEL/model.onnx $P/
install -m644 $MODEL/bert/model.onnx $MODEL/bert/vocab.txt $P/bert/
install -m644 $MODEL/README.md $D/README.md
install -m644 $MODEL/bert/README.md $D/README.bert.md
# the best entry of each word, sorted: sd_vosk maps it instead of building
# a copy in every user's cache (the source dictionary is left out)
$HERE/vosk-test --sort-dictionary $MODEL/dictionary $P/dictionary.sorted
chmod 644 $P/dictionary.sorted
cat > $D/copyright <<'X'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: vosk-model-tts-ru-0.9-multi
Source: https://alphacephei.com/vosk/models

Files: *
Copyright: Alpha Cephei Inc.
License: Apache-2.0

Files: usr/share/vosk-tts/vosk-model-tts-ru-0.9-multi/bert/*
Copyright: SberDevices (ruBert-base)
License: Apache-2.0

License: Apache-2.0
 On Debian systems the full text of the Apache License 2.0 is in
 /usr/share/common-licenses/Apache-2.0.
Comment: dictionary.sorted is made from the model's dictionary by keeping
 the most probable entry of each word and sorting the words.
X
cat > $W/DEBIAN/control <<X
Package: vosk-tts-model-ru-0.9-multi
Version: $MVER
Architecture: all
Maintainer: Grigory Tuboltsev <gtubolcev@gmail.com>
Section: sound
Priority: optional
Installed-Size: $(du -sk $W/usr | cut -f1)
Description: Russian multi-speaker Vosk TTS model
 The Russian neural voice model of Vosk TTS 0.9 (Alpha Cephei): 5 voices,
 a stress dictionary and the ruBert-base model (SberDevices) for the
 sentence intonation. Used by speech-dispatcher-vosk.
X
dpkg-deb --root-owner-group -Zxz -z1 -b $W $OUT/vosk-tts-model-ru-0.9-multi_${MVER}_all.deb
rm -rf "$OUT/root"
ls -la $OUT
