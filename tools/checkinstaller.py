"""checkinstaller.py - a quick sanity check of Commodore Installer scripts (host side):
balanced parentheses outside strings and comments, strings closed, no tabs in strings.

    python tools/checkinstaller.py install/Install_NilBBS out/release/Doors/*/Install_*
"""
import sys

def check(path):
    s = open(path, encoding='latin-1').read()
    depth, i, line, instr, errors, mind = 0, 0, 1, False, [], 0
    while i < len(s):
        c = s[i]
        if c == '\n':
            line += 1
        if instr:
            if c == '\\': i += 2; continue
            if c == '"': instr = False
        elif c == ';':
            while i < len(s) and s[i] != '\n': i += 1
            continue
        elif c == '"':
            instr = True; start = line
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth < 0: errors.append('line %d: a ) too many' % line); depth = 0
        i += 1
    if instr: errors.append('a string opened on line %d is never closed' % start)
    if depth: errors.append('%d ( never closed' % depth)
    return errors

bad = 0
for p in sys.argv[1:]:
    e = check(p)
    print(('OK   ' if not e else 'FAIL ') + p + ('' if not e else ': ' + '; '.join(e)))
    bad += bool(e)
sys.exit(1 if bad else 0)
