#!/bin/sh
# Builds speech-dispatcher-kokoro (sd_kokoro, the default config and an
# acute-aware Russian espeak-ng dictionary) and kokoro-tts-model-ru (the
# kokoro-ru model of zaakirio with its three voices) here.
# MODEL: the download of huggingface.co/zaakirio/kokoro-ru;
# RU_DICT: ru_dict compiled from espeak-ng master with this system's espeak-ng.
set -e
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
K=/home/grigory/Storage/Claude/kokoro-ru
MODEL=${MODEL:-$K/model}
RU_DICT=${RU_DICT:-$K/data/espeak-ng-data/ru_dict}
OUT=${OUT:-$K/deb}
VER=0.1-1~local
MVER=0.1-1~local
SYS=/usr/lib/x86_64-linux-gnu/espeak-ng-data
rm -rf "$OUT/root"; mkdir -p "$OUT/root"

MIT='License: MIT
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
 SOFTWARE.'

# ------------------------------------------------------------ the module
W=$OUT/root/speech-dispatcher-kokoro
D=$W/usr/share/doc/speech-dispatcher-kokoro
E=$W/usr/share/speech-dispatcher-kokoro/espeak-ng-data
mkdir -p $W/DEBIAN $W/usr/lib/speech-dispatcher-modules $W/etc/speech-dispatcher/modules $D $E
$HERE/build.sh
install -m755 $HERE/sd_kokoro $W/usr/lib/speech-dispatcher-modules/sd_kokoro
strip $W/usr/lib/speech-dispatcher-modules/sd_kokoro
install -m644 $HERE/kokoro.conf $W/etc/speech-dispatcher/modules/kokoro.conf
echo /etc/speech-dispatcher/modules/kokoro.conf > $W/DEBIAN/conffiles
# espeak-ng's data with our ru_dict: everything else links to the system's
install -m644 $RU_DICT $E/ru_dict
for f in $SYS/*; do
  b=$(basename $f)
  [ $b = ru_dict ] || ln -s $f $E/$b
done
cat > $D/copyright <<X
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: sd_kokoro

Files: *
Copyright: 2026 Grigory Tuboltsev
$MIT
Comment: ru-g2p.c is a port of ru_g2p.py of zaakirio/kokoro-ru (Apache-2.0),
 of misaki's espeak front end (Apache-2.0) and of the punctuation handling of
 phonemizer (GPL-3+). Linked with libespeak-ng (GPL-3+) and statically with
 libspeechd_module from speech-dispatcher (LGPL-2.1+); as a whole the module
 is under GPL-3+. The module source is in this directory (*.c.gz, *.h).

Files: usr/share/speech-dispatcher-kokoro/espeak-ng-data/ru_dict
Copyright: espeak-ng contributors
License: GPL-3+
 On Debian systems the full text of the GNU General Public License version 3
 is in /usr/share/common-licenses/GPL-3.
Comment: compiled from ru_rules, ru_list and ru_emoji of espeak-ng master
 (688cb60), which honour the stress mark U+0301.
X
for f in sd_kokoro.c kokoro.c ru-g2p.c; do gzip -9n -c $HERE/$f > $D/$f.gz; done
install -m644 $HERE/kokoro.h $HERE/ru-g2p.h $D/
cat > $W/DEBIAN/control <<X
Package: speech-dispatcher-kokoro
Version: $VER
Architecture: amd64
Maintainer: Grigory Tuboltsev <gtubolcev@gmail.com>
Section: sound
Priority: optional
Depends: libc6, libglib2.0-0t64, libjson-glib-1.0-0, libonnxruntime1.23,
 libespeak-ng1, espeak-ng-data, speech-dispatcher (>= 0.12),
 kokoro-tts-model-ru, vosk-tts-model-ru-0.9-multi
Installed-Size: $(du -sk --total $W/usr $W/etc | tail -1 | cut -f1)
Description: Speech Dispatcher module for Kokoro voices
 Speaks with Kokoro neural voices through ONNX Runtime, without Python.
 So far Russian (the kokoro-ru model): stress marks from the dictionary of
 Vosk TTS, phonemes from espeak-ng with a dictionary that honours them.
 The model is loaded into the module once and stays loaded.
 Settings in /etc/speech-dispatcher/modules/kokoro.conf.
X
dpkg-deb --root-owner-group -b $W $OUT/speech-dispatcher-kokoro_${VER}_amd64.deb

[ -n "$MODULE_ONLY" ] && exit 0

# ------------------------------------------------------------- the model
W=$OUT/root/kokoro-tts-model-ru
P=$W/usr/share/kokoro-tts/kokoro-ru
D=$W/usr/share/doc/kokoro-tts-model-ru
mkdir -p $W/DEBIAN $P/voices $D
install -m644 $MODEL/config.json $P/
install -m644 $MODEL/onnx/model.onnx $P/model.onnx
install -m644 $MODEL/onnx/model_dima.onnx $P/dima.onnx
install -m644 $MODEL/voices/sveta.bin $MODEL/voices/masha.bin $MODEL/voices/dima.bin $P/voices/
cat > $P/voices.txt <<'X'
# <name> <male|female> [<onnx file, default model.onnx>]
sveta female
masha female
dima male dima.onnx
X
install -m644 $MODEL/README.md $D/README.md
cat > $D/copyright <<'X'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: kokoro-ru
Source: https://huggingface.co/zaakirio/kokoro-ru

Files: *
Copyright: zaakirio; Kokoro-82M by hexgrad (Apache-2.0)
License: OpenRAIL
 The weights are released under an OpenRAIL license (see README.md); its
 use restrictions apply to everyone who uses these voices. The voices come
 from the Dialogs corpus (studio recordings of consenting actors).
Comment: model.onnx and dima.onnx are onnx/model.onnx and
 onnx/model_dima.onnx (fp32: faster on a CPU than the quantized ones).
X
cat > $W/DEBIAN/control <<X
Package: kokoro-tts-model-ru
Version: $MVER
Architecture: all
Maintainer: Grigory Tuboltsev <gtubolcev@gmail.com>
Section: sound
Priority: optional
Installed-Size: $(du -sk $W/usr | cut -f1)
Description: Russian Kokoro voice models
 kokoro-ru (zaakirio), a Russian fine-tune of Kokoro-82M: the voices
 sveta and masha (female) and dima (male, a model of its own). Used by
 speech-dispatcher-kokoro.
X
dpkg-deb --root-owner-group -Zzstd -z3 -b $W $OUT/kokoro-tts-model-ru_${MVER}_all.deb
rm -rf "$OUT/root"
ls -la $OUT
