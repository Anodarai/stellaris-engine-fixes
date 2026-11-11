"""Check the native wide arithmetic against Python's independent, unbounded integers."""
import itertools
import pathlib
import random
import subprocess
import tempfile


def expected(a, b, divisor, mode):
    product = a * b
    negative = (product < 0) != (divisor < 0)
    q, r = divmod(abs(product), abs(divisor))
    if mode == 1 and negative and r:
        q += 1  # floor
    elif mode == 2 and 2 * r >= abs(divisor):
        q += 1  # nearest, ties away from zero
    result = -q if negative else q
    return max(-(1 << 63), min((1 << 63) - 1, result))


def main():
    root = pathlib.Path(__file__).resolve().parents[1]
    executable = root / 'build/numeric_test.exe'
    edges = [-(1 << 63), -(1 << 63) + 1, -100001, -100000, -32768,
             -1, 0, 1, 32768, 100000, 100001, (1 << 63) - 2, (1 << 63) - 1]
    rng = random.Random(3003)
    with tempfile.TemporaryDirectory(prefix='numeric-oracle-', dir=executable.parent) as temporary:
        path = pathlib.Path(temporary) / 'oracle.txt'
        with path.open('w', encoding='ascii') as out:
            def write(a, b, divisor):
                if divisor:
                    for mode in range(3):
                        out.write(f'{a} {b} {divisor} {mode} {expected(a, b, divisor, mode)}\n')
            for a, b, divisor in itertools.product(edges, repeat=3):
                write(a, b, divisor)
            for _ in range(50000):
                a, b, divisor = (rng.randrange(-(1 << 63), 1 << 63) for _ in range(3))
                write(a, b, divisor)
                # Fixed-point divisors and operands near the non-saturating range.
                write(a, rng.randrange(-200000, 200001), rng.choice([32768, 100000]))
        subprocess.run([str(executable), '--oracle', str(path)], check=True)
        with path.open('w', encoding='ascii') as out:
            def write_ratio(a,b,scale):
                for mode in range(3):
                    out.write(f'{a & ((1<<64)-1)} {a>>64} {b & ((1<<64)-1)} {b>>64} {scale} {mode} {expected(a,scale,b,mode)}\n')
            wide_edges=edges+[-(1<<127),(1<<127)-1,-(1<<95),(1<<95)-1]
            for a,b,scale in itertools.product(wide_edges,wide_edges,[0,1,32768,100000,(1<<63)-1,(1<<64)-1]):
                if b: write_ratio(a,b,scale)
            for _ in range(30000):
                a,b=(rng.randrange(-(1<<127),1<<127) for _ in range(2))
                if b:write_ratio(a,b,rng.choice([1,32768,100000,rng.randrange(1<<64)]))
        subprocess.run([str(executable),'--ratio-oracle',str(path)],check=True)


if __name__ == '__main__':
    main()
