"""A small MSVC name demangler: RTTI class names (namespaces, back references, template arguments) and the common
member-function forms of imports. Enough for readable names, not a full undname."""
import re

PRIM = {'C': 'schar', 'D': 'char', 'E': 'uchar', 'F': 'short', 'G': 'ushort', 'H': 'int', 'I': 'uint', 'J': 'long',
        'K': 'ulong', 'M': 'float', 'N': 'double', 'X': 'void', '_N': 'bool', '_J': 'int64', '_K': 'uint64', '_W': 'wchar_t'}


class Dm:
    def __init__(self, s): self.s, self.p = s, 0

    def num(self):
        s = self.s
        neg = s[self.p] == '?'
        if neg: self.p += 1
        c = s[self.p]
        if c.isdigit():
            self.p += 1; v = int(c) + 1
        else:
            e = s.index('@', self.p); v = 0
            for ch in s[self.p:e]: v = v * 16 + (ord(ch) - 65)
            self.p = e + 1
        return -v if neg else v

    def typ(self, br):
        s = self.s
        c = s[self.p]
        if c in 'VUT':
            self.p += 1; return self.full(br)
        if s.startswith('W4', self.p):
            self.p += 2; return self.full(br)
        if s.startswith('$0', self.p):
            self.p += 2; return str(self.num())
        if c in 'PQ' and s[self.p + 1] in 'ABCD':
            self.p += 2; return self.typ(br) + ('*' if c == 'P' else '* const')
        for k in ('_N', '_J', '_K', '_W'):
            if s.startswith(k, self.p):
                self.p += 2; return PRIM[k]
        if c in PRIM:
            self.p += 1; return PRIM[c]
        raise ValueError('type ' + s[self.p:])

    def frag(self, br):
        s = self.s
        c = s[self.p]
        if c.isdigit():
            self.p += 1; return br[int(c)]
        if s.startswith('?$', self.p):
            self.p += 2
            e = s.index('@', self.p); nm = s[self.p:e]; self.p = e + 1
            tbr, args = [nm], []
            while s[self.p] != '@':
                a = self.typ(tbr); args.append(a)
                if len(tbr) < 10 and not a[0].isdigit(): tbr.append(a)
            self.p += 1
            f = '%s<%s>' % (nm, ','.join(args))
        else:
            e = s.index('@', self.p); f = s[self.p:e]; self.p = e + 1
        if len(br) < 10: br.append(f)
        return f

    def full(self, br):
        fr = []
        while self.s[self.p] != '@': fr.append(self.frag(br))
        self.p += 1
        return '::'.join(reversed(fr))


def demangle_type(s):
    """'.?AVFoo@Bar@@' -> 'Bar::Foo'"""
    try:
        return Dm(s[4:]).full([])
    except (ValueError, IndexError):
        s = s[4:].rstrip('@')
        return '::'.join(reversed([p for p in re.sub(r'[?$]', '', s).split('@') if p]))[:100]


SPECIAL = {'0': 'ctor', '1': 'dtor', '2': 'operator new', '3': 'operator delete', '4': 'operator=',
           '_G': "`scalar deleting dtor'", '_E': "`vector deleting dtor'", '_U': 'operator new[]', '_V': 'operator delete[]'}


def demangle_fn(s):
    try:
        if s.startswith('??'):
            k = '_' + s[3] if s[2] == '_' else s[2]
            rest = s[2 + len(k):]
            if rest.startswith('@'): return SPECIAL.get(k, s)
            cls = Dm(rest).full([])
            last = cls.split('::')[-1]
            sp = SPECIAL.get(k, '?' + k)
            return cls + '::' + {'ctor': last, 'dtor': '~' + last}.get(sp, sp)
        if s.startswith('?'):
            return Dm(s[1:]).full([])
    except (ValueError, IndexError, KeyError):
        pass
    return s
