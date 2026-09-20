import os, subprocess, time, sys

DATA = os.environ.get('EDEN_PROF_DATA', r'F:\prof')

out = open(os.path.join(DATA, 'gpu_watch.csv'), 'w')
out.write('t,gpu_util,sm_clk,mem_clk,temp,mem_used_mib\n')
t0 = time.time()
while time.time() - t0 < float(sys.argv[1] if len(sys.argv) > 1 else 75):
    r = subprocess.run(['nvidia-smi', '--query-gpu=utilization.gpu,clocks.sm,clocks.mem,temperature.gpu,memory.used',
                        '--format=csv,noheader,nounits'], capture_output=True).stdout.decode()
    vals = [v.strip() for v in r.strip().split(',')]
    out.write(f'{time.time()-t0:.1f},{",".join(vals)}\n')
    out.flush()
    time.sleep(1.0)
out.close()
print('done')
