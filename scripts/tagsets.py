#!/usr/bin/env python3
"""Freeze part-of-speech tagsets and their crosswalks to UD's parts of speech as highway lists and edges.

A tagset is a published, persistent notation (the Brown Corpus's tags, the Penn Treebank's, WordNet's letters, the
abbreviations of Webster's Unabridged Dictionary). Each of its tags is a type of its own highway list, identified by
the tag's own text, so the same tag text is the same entity wherever it is written. Its crosswalk to UD's universal
parts of speech (manifest/slots/upos.tsv) is one edge per tag; a tag the crosswalk equates with no universal part of
speech has no edge and remains the tagset's own category.

The crosswalks of the Brown Corpus and the Penn Treebank are NLTK's universal_tagset (Petrov, Das and McDonald, "A
Universal Part-of-Speech Tagset"), en-brown.map and en-ptb.map. They map to the twelve universal tags, which carry to
UD's parts of speech: "." is PUNCT, PRT is PART, CONJ is CCONJ, and the other nine (X among them) keep their names.
WordNet's and Webster's are the source vocabularies the Engine's recipes declared (LIST KEY CANONICAL, '-' for none).

Slots never move: an existing slot file is extended, never reordered; a tag no input lists any longer is an error
here (retire it by hand in a reviewed change). Identifiers are verified by `laplace highway --identities`.
"""
import argparse
import hashlib
from pathlib import Path
import os
import subprocess
import sys
import tempfile

UNIVERSAL = {'VERB': 'VERB', 'NOUN': 'NOUN', 'PRON': 'PRON', 'ADJ': 'ADJ', 'ADV': 'ADV', 'ADP': 'ADP',
             'CONJ': 'CCONJ', 'DET': 'DET', 'NUM': 'NUM', 'PRT': 'PART', 'X': 'X', '.': 'PUNCT'}
LISTS = {'brown': 'Brown Corpus part-of-speech tag', 'ptb': 'Penn Treebank part-of-speech tag',
         'wordnet-pos': 'WordNet part of speech', 'webster-pos': "Webster's Unabridged part of speech"}
ZERO = '0' * 32


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def nltk_map(path):
    """KEY<TAB>UNIVERSAL lines, in the published order."""
    rows = []
    for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip():
            continue
        fields = line.split('\t')
        if len(fields) != 2 or fields[1] not in UNIVERSAL or not fields[0]:
            sys.exit(f'{path}:{number}: expected TAG<TAB>UNIVERSAL')
        rows.append((fields[0], UNIVERSAL[fields[1]]))
    return rows


def words(line):
    out, at = [], 0
    while at < len(line):
        if line[at] in ' \t':
            at += 1
        elif line[at] == '"':
            end = line.index('"', at + 1)
            out.append(line[at + 1:end]); at = end + 1
        else:
            end = at
            while end < len(line) and line[end] not in ' \t':
                end += 1
            out.append(line[at:end]); at = end
    return out


def vocabulary(path):
    """The Engine's recipe vocabulary format: 'list upos', then KEY CANONICAL ('-' for none)."""
    rows, header = [], False
    for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        line = line.split('#', 1)[0] if not line.lstrip().startswith('"') else line
        w = words(line)
        if not w:
            continue
        if len(w) != 2:
            sys.exit(f'{path}:{number}: expected KEY CANONICAL')
        if not header:
            if w != ['list', 'upos']:
                sys.exit(f'{path}: expected list upos')
            header = True
            continue
        rows.append((w[0], None if w[1] == '-' else w[1]))
    return rows


def frozen(path):
    rows = []
    if path.exists():
        for line in path.read_text(encoding='utf-8').splitlines():
            if line.startswith('#') or line.startswith('slot\t'):
                continue
            rows.append(line.split('\t'))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--engine', type=Path, required=True, help='laplace (computes the identifiers)')
    parser.add_argument('--universal', type=Path, required=True, help="NLTK's taggers/universal_tagset directory")
    parser.add_argument('--wordnet-pos', type=Path, required=True, help='the vocabulary of WordNet letters')
    parser.add_argument('--webster-pos', type=Path, required=True, help="the vocabulary of Webster's abbreviations")
    parser.add_argument('--manifest', type=Path, default=Path(__file__).resolve().parents[1] / 'manifest')
    args = parser.parse_args()
    slots = args.manifest / 'slots'
    upos = {row[3]: int(row[0]) for row in frozen(slots / 'upos.tsv')}
    brown_map, ptb_map = args.universal / 'en-brown.map', args.universal / 'en-ptb.map'
    # list -> [(key as the source writes it, identifier, UPOS or None)], plus the header's provenance
    tagsets = {
        # The corpus writes its tags in lower case (nn-tl); the tagset and its crosswalk write them in capitals.
        'brown': ([(tag.lower(), tag, pos) for tag, pos in nltk_map(brown_map)],
                  f'NLTK universal_tagset en-brown.map sha256 {digest(brown_map)}'),
        'ptb': ([(tag, tag, pos) for tag, pos in nltk_map(ptb_map)],
                f'NLTK universal_tagset en-ptb.map sha256 {digest(ptb_map)}'),
        'wordnet-pos': ([(key, key, pos) for key, pos in vocabulary(args.wordnet_pos)],
                        'the WordNet and WN-LMF part-of-speech letters (Laplace-Engine recipes/vocabularies/wordnet-pos.tsv)'),
        'webster-pos': ([(key, key, pos) for key, pos in vocabulary(args.webster_pos)],
                        "Webster's Unabridged (1913) part-of-speech abbreviations (Laplace-Engine recipes/vocabularies/webster-pos.tsv)"),
    }
    edges = []
    with tempfile.TemporaryDirectory() as scratch:
        manifest = Path(scratch)
        (manifest / 'slots').mkdir()
        (manifest / 'slots' / 'lists.tsv').write_text(
            'list\tlabel\n' + ''.join(f'{name}\t{say}\n' for name, say in LISTS.items()), encoding='utf-8')
        planned = {}
        for name, (rows, _) in tagsets.items():
            keys = [key for key, _, _ in rows]
            if len(set(keys)) != len(keys):
                sys.exit(f'{name}: a key names two tags')
            for key, _, pos in rows:
                if pos is not None and pos not in upos:
                    sys.exit(f'{name} {key}: {pos} is not a UD part of speech')
            old = frozen(slots / f'{name}.tsv')
            order = [row[3] for row in old]
            missing = [key for key in order if key not in keys]
            if missing:
                sys.exit(f'{name}: frozen tags no input lists any longer: {missing[:5]}')
            order += [key for key in keys if key not in order]
            by_key = {key: (identifier, pos) for key, identifier, pos in rows}
            planned[name] = [(key,) + by_key[key] for key in order]
            with (manifest / 'slots' / f'{name}.tsv').open('w', encoding='utf-8', newline='\n') as out:
                for slot, (key, identifier, _) in enumerate(planned[name]):
                    out.write(f'{slot}\t{ZERO}\tlive\t{key}' + (f'\t{identifier}' if identifier != key else '') + '\n')
        environment = dict(os.environ, LAPLACE_MANIFEST=str(manifest))
        report = subprocess.run([str(args.engine), 'highway', '--identities'], env=environment, check=True,
                                capture_output=True, text=True, encoding='utf-8').stdout.splitlines()
    canonical = {}
    for line in report[1:]:
        name, slot, _, identifier = line.split('\t')
        canonical[(name, int(slot))] = identifier
    for name, rows in planned.items():
        say, (_, provenance) = LISTS[name], tagsets[name]
        with (slots / f'{name}.tsv').open('w', encoding='utf-8', newline='\n') as out:
            out.write(f'# The frozen slots of the highway list {name} ({say}), from {provenance}.\n'
                      '# A slot never moves; a new tag takes the next. key: the tag as sources write it; a fifth column is\n'
                      '# the tag as the tagset writes it, its identifier. Written by scripts/tagsets.py; never edited by hand.\n'
                      'slot\tid\tstatus\tkey\n')
            for slot, (key, identifier, pos) in enumerate(rows):
                out.write(f'{slot}\t{canonical[(name, slot)]}\tlive\t{key}' + (f'\t{identifier}' if identifier != key else '') + '\n')
                if pos is not None:
                    edges.append((name, slot, upos[pos]))
    with (slots / 'edges-tagsets.tsv').open('w', encoding='utf-8', newline='\n') as out:
        out.write('# Part-of-speech tagset crosswalks: each tag of a tagset to the UD part of speech it is equated with\n'
                  '# (a tag with none has no edge). Written by scripts/tagsets.py; never edited by hand.\n'
                  'from_list\tfrom_slot\tto_list\tto_slot\n')
        for name, slot, target in edges:
            out.write(f'{name}\t{slot}\tupos\t{target}\n')
    lists = slots / 'lists.tsv'
    text = lists.read_text(encoding='utf-8')
    present = {line.split('\t')[0] for line in text.splitlines()}
    with lists.open('a', encoding='utf-8', newline='\n') as out:
        for name, say in LISTS.items():
            if name not in present:
                out.write(f'{name}\t{say}\n')
    for name, rows in planned.items():
        linked = sum(1 for _, _, pos in rows if pos is not None)
        print(f'{name}: {len(rows)} tags, {linked} with a UD part of speech, {len(rows) - linked} without')


if __name__ == '__main__':
    main()
