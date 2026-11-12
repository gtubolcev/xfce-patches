import subprocess, sys, time, os
def run(text):
    p = subprocess.Popen(['./sd_zonos3', 'test-fail.conf'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=open('proto.err', 'a'), bufsize=0)
    def send(s): p.stdin.write(s.encode())
    def line(): return p.stdout.readline().decode(errors='replace').rstrip()
    t0 = time.time()
    send('INIT\n'); 
    while True:
        l = line()
        if l.startswith('299') or l.startswith('399'): break
    send('AUDIO\naudio_output_method=server\n.\n'); 
    while not line().startswith('203'): pass
    send('SET\nvoice=zonos3-ru-1\nlanguage=ru\n.\n'); line()
    send('SPEAK\n'); line(); send('<speak>%s</speak>\n.\n' % text)
    first = None; n = 0
    while True:
        l = line()
        if l.startswith('705-AUDIO'):
            # skip binary until "705 AUDIO" reply needed: read rest of message
            pass
        if l.startswith('705 AUDIO'):
            pass
        if l.startswith('705-num_samples'):
            n += 1
            if first is None: first = time.time() - t0
        if l.startswith('702') or l.startswith('703'):
            break
    total = time.time() - t0
    send('QUIT\n'); p.wait(10)
    print(f'first audio {first:.1f}s, total {total:.1f}s, tracks {n}')
run(sys.argv[1])
