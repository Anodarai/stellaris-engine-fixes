"""Independent unbounded-integer oracle for exact income-target intermediates."""
import itertools
import pathlib
import random
import subprocess
import tempfile

root=pathlib.Path(__file__).resolve().parents[1]
exe=root/'build/incomescaling_test.exe'
edges=[-(1<<127),-(1<<64),-(1<<63),-100000,-1,0,1,100000,(1<<63)-1,1<<64,(1<<127)-1]
rng=random.Random(71329)
with tempfile.TemporaryDirectory(prefix='big-oracle-',dir=exe.parent) as directory:
    path=pathlib.Path(directory)/'oracle.txt'
    with path.open('w',encoding='ascii') as out:
        def write(a,b):
            for op,value in [('+',a+b),('-',a-b),('*',a*b),('c',max(-(1<<63),min((1<<63)-1,a))),('a',a+a),('s',0)]:
                out.write(f'{op} {a} {b} {value}\n')
            if b:
                q=abs(a)//abs(b)
                if (a<0)!=(b<0):q=-q
                out.write(f'/ {a} {b} {q}\n% {a} {b} {a-q*b}\n')
                out.write(f'f {a} {abs(b)} {a//abs(b)}\n')
        for a,b in itertools.product(edges,repeat=2):write(a,b)
        for _ in range(8000):
            a=rng.getrandbits(rng.randrange(1,1025));b=rng.getrandbits(rng.randrange(1,1025))
            write(-a if rng.randrange(2) else a,-b if rng.randrange(2) else b)
    subprocess.run([str(exe),'--oracle',str(path)],check=True)
