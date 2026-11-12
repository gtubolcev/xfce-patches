import subprocess, sys
def run(lang, voicetype):
    p = subprocess.Popen(['./sd_zonos3', 'test-voice.conf'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    def send(s): p.stdin.write(s.encode())
    def line(): return p.stdout.readline().decode(errors='replace').rstrip()
    send('INIT\n')
    while True:
        l = line()
        if l.startswith('299') or l.startswith('399'): break
    send('AUDIO\naudio_output_method=server\n.\n')
    while not line().startswith('203'): pass
    send('SET\nvoice=%s\nsynthesis_voice=NULL\nlanguage=%s\n.\n' % (voicetype, lang)); line()
    send('SPEAK\n'); line(); send('<speak>test</speak>\n.\n')
    import time; time.sleep(3)
    p.kill(); err = p.stderr.read().decode(errors='replace')
    print(lang, voicetype, [l[l.find('speaking'):][:60] for l in err.splitlines() if 'speaking' in l])
run('ru', 'male2'); run('en', 'male1'); run('de', 'female1')
