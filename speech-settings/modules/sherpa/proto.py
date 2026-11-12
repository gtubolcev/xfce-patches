# drives sd_sherpa through the speechd module protocol; saves the audio to out/*.wav (no playback)
import subprocess, sys, time, wave, os, select
os.makedirs('out', exist_ok=True)
def unesc(b):
    o = bytearray(); i = 0
    while i < len(b):
        if b[i] == 0x7d: o.append(b[i+1] ^ 0x20); i += 2
        else: o.append(b[i]); i += 1
    return bytes(o)
def run(tag, sets, text, conf='test.conf', stop_after=None):
    p = subprocess.Popen(['./sd_sherpa', conf], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open('out/%s.err' % tag, 'w'), bufsize=0)
    def send(s): p.stdin.write(s.encode())
    def line(): return p.stdout.readline()
    t0 = time.time()
    send('INIT\n')
    while True:
        l = line().decode(errors='replace')
        if l[:3] in ('299', '399'): print(tag, 'init:', l.strip()); break
    send('AUDIO\naudio_output_method=server\n.\n')
    while not line().startswith(b'203'): pass
    send('SET\n' + ''.join('%s=%s\n' % kv for kv in sets.items()) + '.\n'); line()
    send('SPEAK\n'); line(); send('<speak>%s</speak>\n.\n' % text)
    pcm = b''; sr = 0; first = None; tracks = 0
    while True:
        if stop_after and time.time() - t0 > stop_after:
            send('STOP\n'); stop_after = None
        r, _, _ = select.select([p.stdout], [], [], 120)
        if not r: print(tag, 'TIMEOUT'); break
        l = line()
        if not l: break
        if l.startswith(b'705-bits'):
            f = l.decode().strip().split('-', 1)[1].split('\n')
            hdr = dict(x.split('=') for x in l.decode().strip()[4:].split())if False else None
        if l.startswith(b'705-AUDIO'):
            data = l[len(b'705-AUDIO\0'):]
            while not data.endswith(b'\n705 AUDIO\n') :
                data += line()
            data = unesc(data[:-len(b'\n705 AUDIO\n')])
            pcm += data; tracks += 1
            if first is None: first = time.time() - t0
        elif l.startswith(b'705-'):
            for kv in l.decode().strip()[4:].split('/') if False else []: pass
            if b'sample_rate=' in l: sr = int(l.split(b'sample_rate=')[1].split()[0])
        elif l.startswith(b'702') or l.startswith(b'703'):
            ev = l.decode().strip()
            if ev.startswith('702 END') or ev.startswith('703 STOP'): print(tag, ev); break
    total = time.time() - t0
    p.stdin.write(b'QUIT\n'); p.wait(10)
    if pcm:
        w = wave.open('out/%s.wav' % tag, 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr or 44100); w.writeframes(pcm); w.close()
    dur = len(pcm) / 2 / (sr or 44100)
    print('%s: %d track(s), %.2f s audio, first audio after %s s, total %.1f s, sr %d' % (tag, tracks, dur, '%.1f' % first if first else '-', total, sr))
