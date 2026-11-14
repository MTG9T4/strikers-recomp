"""Audit actual host binding tables, never infer coverage from an import's namespace.

GPL-3.0-or-later. Strikers project code. No third-party Python packages required.
Status is static binding coverage, not a claim of SDK ABI or runtime correctness.
"""
import argparse
import ast
import collections
import csv
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
STRING = r'"(?:\\.|[^"\\])*"'


def uncomment(text):
    return re.sub(rf'{STRING}|/\*[\s\S]*?\*/|//[^\n]*',
                  lambda m: m[0] if m[0].startswith('"') else ' ', text)


def bindings(root=ROOT):
    """Expand literal prefix macros such as OS("...") and adjacent C strings.

    Only ImportDef initializers are considered; strings in tests/comments do not count.
    The project uses literal names or one-argument literal prefix macros in these tables.
    Unrecognised initializers fail the audit instead of disappearing from it.
    """
    found = {}
    for path in sorted((root / 'runtime').rglob('*.c')):
        text = uncomment(path.read_text(encoding='utf-8'))
        for name, address in re.findall(r'\bimport_set_address\(\s*("[^"\n]*")\s*,\s*([^;]+?)\);', text):
            found.setdefault(ast.literal_eval(name), []).append({
                'file': path.relative_to(root).as_posix(), 'handler': 'data:' + address.strip()})
        macros = dict(re.findall(r'^\s*#define\s+(\w+)\(\w+\)\s+("[^"\n]*")\s+\w+\s*$', text, re.M))
        literal_defs = dict(re.findall(r'^\s*#define\s+(\w+)\s+("[^"\n]*")\s*$', text, re.M))
        for table in re.finditer(r'\bImportDef\s+(\w+)\s*\[\s*\]\s*=\s*\{([\s\S]*?)\n\s*\};', text):
            body = table[2]
            for macro, prefix in macros.items():
                body = re.sub(r'\b' + macro + r'\(\s*(' + STRING + r')\s*\)',
                              lambda m: prefix + ' ' + m[1], body)
            body = re.sub(STRING + r'|\b\w+\b',
                          lambda m: literal_defs.get(m[0], m[0]), body)
            entries = re.findall(r'\{\s*((?:' + STRING + r'\s*)+)\s*,\s*(\w+)\s*\}', body)
            if len(entries) != body.count('{'):
                raise ValueError(f'{path}: unsupported binding initializer')
            for literals, handler in entries:
                name = ''.join(ast.literal_eval(s) for s in re.findall(STRING, literals))
                found.setdefault(name, []).append({'file': path.relative_to(root).as_posix(), 'table': table[1], 'handler': handler})
    return found


def audit(root=ROOT, import_dir=None):
    tables = bindings(root)
    from sdklift_inventory import inventory
    sdk = inventory(root)
    policy_path = root / 'runtime/coverage_policy.json'
    policy = json.loads(policy_path.read_text()) if policy_path.exists() else {}
    rows = []
    for path in sorted((import_dir or root / 'build/dump/update/imports').glob('*_imports.tsv')):
        for row in csv.DictReader(path.open(encoding='utf-8'), delimiter='\t'):
            name = row['name']
            candidates = list(tables.get(name, []))
            # These install paths register MAIN imports. Other modules need their
            # own loader/link closure and must not inherit main's coverage.
            generated = (sdk['functions'].get(name) or sdk['data'].get(name)) if path.name == 'main_imports.tsv' else None
            if generated:
                if name in sdk['functions']:
                    candidates.append({**generated, 'table': 'sdk_install_names'})
                else:
                    candidates.append({**generated, 'handler': 'data:' + generated['handler']})
            rule = policy.get('symbols', {}).get(name)
            if rule:
                status, reason = rule['status'], rule['reason']
            elif candidates:
                # Replay the checked-in registration sequence, including fill-only tables.
                chosen = next((c for c in candidates if c['handler'].startswith('data:')), None)
                for step in policy.get('binding_order', []):
                    for candidate in candidates:
                        key = candidate['file'] + ':' + candidate.get('table', '@data')
                        if key == step['table'] and (chosen is None or step.get('mode') != 'fill'):
                            chosen = candidate
                if chosen is None:
                    status, reason = 'missing', 'binding table is not in the active boot registration sequence'
                    rows.append({'module': path.name.removesuffix('_imports.tsv'),
                                 'namespace': namespace(row['namespace']), 'name': name,
                                 'defined_in': row['defined_in'], 'status': status, 'reason': reason})
                    continue
                status = 'offline' if chosen['file'] in policy.get('offline_files', []) else 'implemented'
                reason = chosen['file'] + ':' + chosen['handler']
                if chosen['handler'] in policy.get('missing_handlers', []):
                    status = 'missing'
                    reason += ' (placeholder)'
            else:
                status, reason = 'missing', 'no host binding (SDK lift is not assumed)'
            if status in ('implemented', 'offline') and not candidates and not generated and not (
                    rule and rule.get('unbound_weak') and row['defined_in'] == '?' and name.startswith('glslc')):
                raise ValueError(f'{name}: policy claims coverage without a binding')
            rows.append({'module': path.name.removesuffix('_imports.tsv'),
                         'namespace': namespace(row['namespace']), 'name': name,
                         'defined_in': row['defined_in'], 'status': status, 'reason': reason})
    if not rows:
        raise ValueError('no import TSV rows found')
    return rows


def namespace(value):
    value = value.strip()
    if value.startswith('(C)'):
        return '(C)'
    if value.startswith('movie::'):
        return 'movie'
    if value.startswith('nv::'):
        return 'nv'
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module', help='filter the importing module, e.g. main')
    parser.add_argument('--summary', action='store_true', help='only print counts')
    parser.add_argument('--json', type=Path, help='save the complete audit as JSON')
    parser.add_argument('--check', action='store_true', help='fail if any selected import is missing')
    args = parser.parse_args()
    rows = audit()
    if args.module:
        rows = [r for r in rows if r['module'] == args.module]
        if not rows:
            parser.error('module has no imports')
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(rows, indent=2) + '\n', encoding='utf-8')
    if not args.summary:
        print('module\tnamespace\tname\tdefined_in\tstatus\treason')
        for row in rows:
            print('\t'.join(row.values()))
    print('\nmodule\tnamespace\timplemented\toffline\tmissing')
    counts = collections.defaultdict(collections.Counter)
    for row in rows:
        counts[row['module'], row['namespace']][row['status']] += 1
    for (module, namespace), c in sorted(counts.items()):
        print(f'{module}\t{namespace}\t{c["implemented"]}\t{c["offline"]}\t{c["missing"]}')
    total = collections.Counter(r['status'] for r in rows)
    print(f'TOTAL\t{len(rows)}\t{total["implemented"]}\t{total["offline"]}\t{total["missing"]}')
    return int(args.check and total['missing'] > 0)


if __name__ == '__main__':
    raise SystemExit(main())
