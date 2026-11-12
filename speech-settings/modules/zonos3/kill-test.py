import subprocess, sys, time, os
mode = sys.argv[1]   # quit | kill
p = subprocess.Popen(['./sd_zonos3', 'test.conf'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=open('proto.err', 'a'), bufsize=0)
def send(s): p.stdin.write(s.encode())
def line(): return p.stdout.readline().decode(errors='replace').rstrip()
send('INIT\n')
while not line()[:3] in ('299', '399'): pass
send('AUDIO\naudio_output_method=server\n.\n')
while not line().startswith('203'): pass
send('SPEAK\n'); line(); send('<speak>Проверка.</speak>\n.\n')
time.sleep(2)
if mode == 'quit':
    send('QUIT\n'); t = time.time(); p.wait(10); print('module exited after QUIT in %.1fs' % (time.time() - t))
else:
    p.kill(); p.wait(); print('module killed')
