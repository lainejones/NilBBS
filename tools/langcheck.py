#!/usr/bin/env python3
"""langcheck.py - sanity-check a NilBBS language file against English.lng.

    python tools/langcheck.py [file.lng ...]      (no argument: English.lng itself)

Parses a .lng file exactly the way src/common/lang.c does and reports:
  - keys that English.lng doesn't have (a typo - the line would never be used)
  - lines whose printf conversions (%s %ld ...) differ from the English: the node
    ignores those and shows the English, so they're errors here
  - keys given twice (the last one wins), and lines that aren't "key = text"
and how much of English.lng the file translates.  Exit status 1 if anything is wrong.
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGLISH = os.path.join(ROOT, 'dist', 'BBS', 'Text', 'Language', 'English.lng')
KEYCH = re.compile(r'[A-Za-z0-9._\-]')


def decode(raw):
    """the text part of a line, as lang.c's decode(): escapes, then UTF-8 or Latin-1 -> str"""
    out, i, n = [], 0, len(raw)
    while i < n:
        c = raw[i]; i += 1
        if c == 0x5C and i < n:                     # backslash
            d = raw[i]; i += 1
            if d == ord('n'): out.append('\n')
            elif d == ord('t'): out.append('\t')
            elif d == ord('x') and i < n and chr(raw[i]) in '0123456789abcdefABCDEF':
                v = int(chr(raw[i]), 16); i += 1
                if i < n and chr(raw[i]) in '0123456789abcdefABCDEF':
                    v = v * 16 + int(chr(raw[i]), 16); i += 1
                if v: out.append('\\x%02X' % v)     # a raw CP437 byte: kept symbolic
            else: out.append(chr(d))
            continue
        if c >= 0x80:
            more = 1 if (c & 0xE0) == 0xC0 else 2 if (c & 0xF0) == 0xE0 else 0
            if more and i + more <= n and all((raw[i + k] & 0xC0) == 0x80 for k in range(more)):
                v = c & (0x1F if more == 1 else 0x0F)
                for k in range(more): v = (v << 6) | (raw[i + k] & 0x3F)
                if v >= 0x80:
                    out.append(chr(v)); i += more; continue
            out.append(chr(c))                      # Latin-1
            continue
        out.append(chr(c))
    return ''.join(out)


def parse(path):
    """-> (entries {key: text}, dups [key], bad [(lineno, line)])"""
    data = open(path, 'rb').read()
    if data.startswith(b'\xef\xbb\xbf'): data = data[3:]
    entries, dups, bad = {}, [], []
    for no, line in enumerate(data.split(b'\n'), 1):
        line = line.rstrip(b'\r \t')
        s = line.lstrip(b' \t')
        if not s or s[:1] in (b';', b'#'): continue
        k = 0
        while k < len(s) and KEYCH.match(chr(s[k])): k += 1
        key = s[:k].decode('ascii')
        rest = s[k:].lstrip(b' \t')
        if not key or not rest.startswith(b'='):
            bad.append((no, line.decode('latin-1'))); continue
        v = rest[1:].lstrip(b' \t')
        if len(v) >= 2 and v[:1] == b'"' and v[-1:] == b'"': v = v[1:-1]
        if key in entries: dups.append(key)
        entries[key] = decode(v)
    return entries, dups, bad


def convs(s):
    """printf conversions as lang.c's next_conv() sees them; '?' = invalid"""
    res, i = [], 0
    while True:
        i = s.find('%', i)
        if i < 0: return res
        i += 1
        if i < len(s) and s[i] == '%': i += 1; continue
        code = ''
        while i < len(s) and s[i] in "-+ #0'": i += 1
        if i < len(s) and s[i] == '*': code += '*'; i += 1
        else:
            while i < len(s) and s[i].isdigit(): i += 1
        if i < len(s) and s[i] == '.':
            i += 1
            if i < len(s) and s[i] == '*': code += '*'; i += 1
            else:
                while i < len(s) and s[i].isdigit(): i += 1
        while i < len(s) and s[i] in 'hlLqjzt' and len(code) < 5: code += s[i]; i += 1
        if i < len(s) and s[i] in 'diouxXcspeEfgGaA': code += s[i]; i += 1
        else:
            code += '?'
            if i < len(s): i += 1
        res.append(code)


def check(path, english):
    entries, dups, bad = parse(path)
    errors = 0
    name = os.path.basename(path)
    for no, line in bad:
        print('%s:%d: not "key = text": %s' % (name, no, line[:60])); errors += 1
    for k in dups:
        print('%s: %s is given more than once (the last one wins)' % (name, k))
    for k, t in entries.items():
        if english is not None and k not in english:
            print('%s: unknown key %s (not in English.lng)' % (name, k)); errors += 1; continue
        e = english[k] if english is not None else t
        ce, ct = convs(e), convs(t)
        if '?' in ''.join(ce):
            print('%s: %s: the English itself has a bad %%-conversion' % (name, k)); errors += 1
        elif ce != ct:
            print('%s: %s: conversions %s, English has %s - the node will show the English'
                  % (name, k, ' '.join('%' + c for c in ct) or 'none', ' '.join('%' + c for c in ce) or 'none'))
            errors += 1
    total = len(english) if english is not None else len(entries)
    have = sum(1 for k in entries if english is None or k in english)
    print('%s: %d keys, %d of %d translated, %d problem(s)' % (name, len(entries), have, total, errors))
    return errors


def main():
    english, dups, bad = parse(ENGLISH)
    errors = 0
    files = sys.argv[1:] or [ENGLISH]
    for f in files:
        same = os.path.abspath(f) == os.path.abspath(ENGLISH)
        errors += check(f, None if same else english)
    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
