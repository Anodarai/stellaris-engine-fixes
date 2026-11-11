"""Build and validate stellaris-engine-fixes against local game executables."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT
BUILD = PROJECT / 'build'
STEAM_452_SHA256 = '400df27c82ddc845aa9dce79bd468d81f18f060d299cd263e3afa93aef4f7a83'
WINDOWS_IMAGE = re.compile(r'stellaris_(\d+)_(steam|gog)\.exe', re.IGNORECASE)
LINUX_IMAGE = re.compile(r'stellaris_?(\d+)_linux', re.IGNORECASE)

UNIT_TESTS = (
    'logging_test', 'proxy_test', 'game_imports_test', 'natsort_test', 'pathcache_test',
    'income_test', 'incomeaccumulator_test', 'pdxstore_test', 'alloctest/alloc_host',
    'mpstore-runtime-test/mpstore_runtime_test',
)
IMAGE_TESTS = (
    'proxy_test', 'locate_test', 'uishortcuts_test', 'mpchecksum_test', 'mpstore_test',
    'incomescaling_test', 'incomeatomic_test', 'incomeproducers_test', 'incomelifetime_test',
    'resourceoverflow_test', 'fixedpointoverflow_test', 'numeric_test', 'resourceledger_test',
    'resourcebatch_test', 'numericdisplay_test', 'diplomacy_test', 'diploscores_test',
    'diploloans_test', 'diplovotes_test', 'diplodisplay_test', 'federationsums_test',
    'shipformula_test', 'shipstats_test', 'componentmath_test', 'shipdamage_test',
    'designcache_test', 'fleetstats_test', 'countrytotals_test', 'forecast_test',
    'numeric-install-test/numeric_install_test',
)
ORACLES = ('test_numeric_math', 'test_numeric_big', 'test_combat_math')
BASELINE_TESTS = {'locate_test', 'mpchecksum_test', 'mpstore_test'}


def discover(version):
    windows, linux = [], []
    for path in sorted((ROOT / 'executables').iterdir()):
        if not path.is_file():
            continue
        for pattern, images in ((WINDOWS_IMAGE, windows), (LINUX_IMAGE, linux)):
            match = pattern.fullmatch(path.name)
            if match and (version is None or match[1] == version):
                images.append(path)
    return windows, linux


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def is_linux_x64(path):
    with path.open('rb') as source:
        header = source.read(64)
    return (len(header) == 64 and header.startswith(b'\x7fELF\x02\x01\x01')
            and struct.unpack_from('<HHI', header, 16) in ((2, 62, 1), (3, 62, 1)))


def run(report_dir, group, name, command, timeout=120):
    log = report_dir / group / f'{name}.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    scratch = str(report_dir / 'tmp')
    with log.open('wb') as output:
        try:
            result = subprocess.run(command, cwd=PROJECT, stdout=output,
                                    stderr=subprocess.STDOUT, timeout=timeout,
                                    env=os.environ | {'TEMP': scratch, 'TMP': scratch})
            exit_code, error = result.returncode, None
        except subprocess.TimeoutExpired:
            exit_code, error = None, f'timed out after {timeout} seconds'
        except OSError as exc:
            exit_code, error = None, str(exc)
        if error:
            output.write(f'\n{error}\n'.encode('utf-8', errors='replace'))
    status = 'PASS' if exit_code == 0 else 'FAIL'
    if status == 'FAIL':
        print(f'  FAIL {group}/{name} ({error or f"exit {exit_code}"}). See {log}', flush=True)
    return {'name': name, 'status': status, 'exit_code': exit_code, 'error': error,
            'log': log.relative_to(report_dir).as_posix()}


def summarize(group, results):
    passed = sum(result['status'] == 'PASS' for result in results)
    print(f'{group} | {passed}/{len(results)} passed', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    images = parser.add_mutually_exclusive_group()
    images.add_argument('--version', help='select a version in executables/ (e.g. 4.5.4 or 454)')
    images.add_argument('--exe', action='append', type=Path, help='select a Windows image; repeat for more')
    parser.add_argument('--skip-build', action='store_true', help='use the existing build outputs')
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('the DLL and native test suite require Windows')
    if args.version is not None and not re.fullmatch(r'[0-9]+(?:\.[0-9]+)*', args.version):
        parser.error('--version must use digits, with optional dots')
    version = args.version.replace('.', '') if args.version else None
    if args.exe:
        windows, linux = list(dict.fromkeys(path.resolve() for path in args.exe)), []
    else:
        try:
            windows, linux = discover(version)
        except OSError as exc:
            parser.error(str(exc))
    if not windows:
        parser.error('no Windows game executables selected')
    for path in windows:
        if not path.is_file():
            parser.error(f'executable not found: {path}')

    # Keep new native tests from silently falling out of the matrix. These two
    # sources are a support DLL and a manual loader probe, respectively.
    registered = {Path(name).name for name in UNIT_TESTS + IMAGE_TESTS}
    sources = {path.stem for path in (PROJECT / 'test').glob('*.cpp')}
    missing = sources - registered - {'alloc_testdll', 'static_import_test'}
    if missing:
        parser.error('register native tests in the validation suite: ' + ', '.join(sorted(missing)))

    BUILD.mkdir(exist_ok=True)
    report_dir = Path(tempfile.mkdtemp(prefix='validation-', dir=BUILD))
    (report_dir / 'tmp').mkdir()
    print(f'Logs and report | {report_dir}', flush=True)
    report = {'build': None, 'proxy_sha256': None, 'unit_tests': [], 'images': []}
    if not args.skip_build:
        print('Building engine fixes and native tests', flush=True)
        report['build'] = run(report_dir, 'build', 'build',
                              [os.environ.get('COMSPEC', 'cmd.exe'), '/d', '/c', 'build.bat'],
                              timeout=None)
    built = args.skip_build or report['build']['status'] == 'PASS'
    failed = not built
    if built:
        proxy = BUILD / 'winmm/winmm.dll'
        if proxy.is_file():
            report['proxy_sha256'] = sha256(proxy)
        results = report['unit_tests']
        for name in UNIT_TESTS:
            results.append(run(report_dir, 'unit', Path(name).name, [BUILD / f'{name}.exe']))
        for name in ORACLES:
            results.append(run(report_dir, 'unit', name, [sys.executable, ROOT / 'tools' / f'{name}.py']))
        summarize('Unit tests and arithmetic oracles', results)
        failed |= any(result['status'] != 'PASS' for result in results)

        for index, path in enumerate(windows, 1):
            digest = sha256(path)
            baseline = digest == STEAM_452_SHA256
            results = []
            report['images'].append({'path': str(path), 'platform': 'windows', 'sha256': digest,
                                     'baseline_rva_checks': baseline, 'tests': results})
            group = f'{index:02d}-{path.stem}'
            for name in IMAGE_TESTS:
                command = [BUILD / f'{name}.exe', path]
                if baseline and name in BASELINE_TESTS:
                    command.append('--expect-known')
                results.append(run(report_dir, group, Path(name).name, command))
            summarize(path.name, results)
            failed |= any(result['status'] != 'PASS' for result in results)

        for path in linux:
            valid = is_linux_x64(path)
            status = 'inventory_only' if valid else 'invalid_elf'
            report['images'].append({'path': str(path), 'platform': 'linux', 'sha256': sha256(path),
                                     'status': status})
            print(f'{path.name} | {"ELF x86-64, inventory only" if valid else "FAIL: expected ELF x86-64"}',
                  flush=True)
            failed |= not valid

    report['status'] = 'FAIL' if failed else 'PASS'
    report_path = report_dir / 'report.json'
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'{report["status"]} | {report_path}')
    return int(failed)


if __name__ == '__main__':
    sys.exit(main())
