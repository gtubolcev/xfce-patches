#!/bin/sh
# Builds speech-dispatcher-sherpa (sd_sherpa + default config) into this directory
set -e
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
VER=0.1-2~local
W=$HERE/deb/speech-dispatcher-sherpa
rm -rf "$HERE/deb"; mkdir -p $W/DEBIAN $W/usr/lib/speech-dispatcher-modules \
    $W/etc/speech-dispatcher/modules $W/usr/share/doc/speech-dispatcher-sherpa
$HERE/build.sh
install -m755 $HERE/sd_sherpa $W/usr/lib/speech-dispatcher-modules/sd_sherpa
strip $W/usr/lib/speech-dispatcher-modules/sd_sherpa
install -m644 $HERE/sherpa.conf $W/etc/speech-dispatcher/modules/sherpa.conf
echo /etc/speech-dispatcher/modules/sherpa.conf > $W/DEBIAN/conffiles
cat > $W/usr/share/doc/speech-dispatcher-sherpa/copyright <<'X'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: sd_sherpa

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
Comment: statically linked with libspeechd_module from speech-dispatcher
 (LGPL-2.1+), which is allowed for the object code; the module source is
 /usr/share/doc/speech-dispatcher-sherpa/sd_sherpa.c.gz
X
gzip -9n -c $HERE/sd_sherpa.c > $W/usr/share/doc/speech-dispatcher-sherpa/sd_sherpa.c.gz
cat > $W/DEBIAN/control <<X
Package: speech-dispatcher-sherpa
Version: $VER
Architecture: amd64
Maintainer: Grigory Tuboltsev <gtubolcev@gmail.com>
Section: sound
Priority: optional
Depends: libc6, libglib2.0-0t64, speech-dispatcher (>= 0.12), sherpa-onnx (>= 2.0.1),
 sherpa-onnx-tts-supertonic-3 | sherpa-onnx-tts-pocket | sherpa-onnx-tts-mms-ru
Recommends: sherpa-onnx-tts-supertonic-3
Installed-Size: $(du -sk --total $W/usr $W/etc | tail -1 | cut -f1)
Description: Speech Dispatcher module for sherpa-onnx voices
 Speaks with neural voices through sherpa-onnx: Supertonic 3 (31 languages,
 among them Russian and English, 5 female and 5 male voices), Pocket TTS
 (English) and MMS (one language per model). The model is loaded into the
 module once and stays loaded, so only the first message waits for it.
 Settings in /etc/speech-dispatcher/modules/sherpa.conf.
X
dpkg-deb --root-owner-group -b $W $HERE/speech-dispatcher-sherpa_${VER}_amd64.deb
