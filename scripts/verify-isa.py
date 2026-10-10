"""Require actual successful execution of every registered native dispatch test."""
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
cases = {case.attrib['name']: case for case in root.iter('testcase')}
expected = {f'{kernel}_{isa}' for kernel in ('follows', 'core', 'coord', 'shape', 's3', 'kernels')
            for isa in ('scalar', 'sse2', 'avx2', 'avxvnni', 'avx512', 'avx512vnni')}
missing = sorted(expected - cases.keys())
unsuccessful = sorted(name for name, case in cases.items()
                      if any(case.find(tag) is not None for tag in ('failure', 'error', 'skipped'))
                      or case.get('status', 'run') not in ('run', 'passed'))
if missing or unsuccessful:
    raise SystemExit(f'ISA verification failed: missing={missing}, unsuccessful={unsuccessful}')
print(f'{len(expected)} required dispatch cases executed; {len(cases)} total tests passed with no skips.')
