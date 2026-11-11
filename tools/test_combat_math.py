"""Independent Decimal oracle for the integer-interval overflow exponential."""
import decimal
import pathlib
import random
import subprocess
import tempfile

root=pathlib.Path(__file__).resolve().parents[1]
exe=root/'build/shipformula_test.exe'
rng=random.Random(71383)
cases=[(x,cap) for x in [-10000,-897,-896,-895,-1,0,1,127,128,129,512,8192,20000]
       for cap in [1,127,128,129,(1<<63)-1,1<<72]]
cases += [(rng.randrange(-1100,20001),rng.randrange(1,1<<rng.randrange(1,74))) for _ in range(1800)]
# Inputs with huge exponents exercise the proven zero/saturation exits.
cases += [(1<<200,1<<72),(-(1<<200),1<<72)]
with tempfile.TemporaryDirectory(prefix='combat-oracle-',dir=exe.parent) as directory:
    path=pathlib.Path(directory)/'oracle.txt'
    with path.open('w',encoding='ascii') as out:
        for x,cap in cases:
            answers=[]
            for precision in (180,260):
                with decimal.localcontext() as context:
                    context.prec=precision
                    if abs(x)>100000:answer=cap+1 if x>0 else 0
                    else:answer=min(cap+1,int((decimal.Decimal(x)/128).exp()*128))
                    answers.append(answer)
            assert answers[0]==answers[1],(x,cap,answers)
            out.write(f'{x} {cap} {answers[0]}\n')
    subprocess.run([str(exe),'--oracle',str(path)],check=True)
