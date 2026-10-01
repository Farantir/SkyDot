# SPDX-License-Identifier: GPL-3.0-or-later
"""Skyrim PEX (compiled Papyrus) reader for analysis, written from UESP's
format description. Big-endian. Not engine code: the engine will read PEX
through SpanReader."""
import struct, sys
OPS = [('nop',0),('iadd',3),('fadd',3),('isub',3),('fsub',3),('imul',3),('fmul',3),('idiv',3),('fdiv',3),('imod',3),
       ('not',2),('ineg',2),('fneg',2),('assign',2),('cast',2),('cmp_eq',3),('cmp_lt',3),('cmp_le',3),('cmp_gt',3),('cmp_ge',3),
       ('jmp',1),('jmpt',2),('jmpf',2),('callmethod',3),('callparent',2),('callstatic',3),('return',1),('strcat',3),
       ('propget',3),('propset',3),('array_create',2),('array_length',2),('array_getelement',3),('array_setelement',3),
       ('array_findelement',4),('array_rfindelement',4)]
VARARGS = {23, 24, 25}
class R:
    def __init__(s, b): s.b=b; s.p=0
    def u(s,f):
        v=struct.unpack_from('>'+f,s.b,s.p); s.p+=struct.calcsize('>'+f); return v[0] if len(v)==1 else v
    def ws(s): n=s.u('H'); t=s.b[s.p:s.p+n].decode('cp1252'); s.p+=n; return t
class Pex:
    def __init__(self, b):
        r=R(b); self.r=r
        assert r.u('I')==0xFA57C0DE, 'magic'
        self.ver=(r.u('B'),r.u('B')); self.game=r.u('H'); r.u('Q'); self.source=r.ws(); r.ws(); r.ws()
        self.strings=[r.ws() for _ in range(r.u('H'))]
        if r.u('B'):
            r.u('Q')
            for _ in range(r.u('H')):
                r.u('H');r.u('H');r.u('H');r.u('B'); n=r.u('H'); r.p+=2*n
        for _ in range(r.u('H')): r.u('H'); r.u('B')
        self.objects=[self.obj() for _ in range(r.u('H'))]
    def s(self): return self.strings[self.r.u('H')]
    def var(self):
        t=self.r.u('B')
        if t==0: return ('null',None)
        if t==1: return ('id',self.s())
        if t==2: return ('str',self.s())
        if t==3: return ('int',self.r.u('i'))
        if t==4: return ('float',self.r.u('f'))
        if t==5: return ('bool',self.r.u('B'))
        raise ValueError('vartype %d'%t)
    def func(self, name):
        r=self.r
        f={'name':name,'ret':self.s(),'doc':self.s(),'uflags':r.u('I'),'flags':r.u('B')}
        f['params']=[(self.s(),self.s()) for _ in range(r.u('H'))]
        f['locals']=[(self.s(),self.s()) for _ in range(r.u('H'))]
        ins=[]
        for _ in range(r.u('H')):
            op=r.u('B'); n=OPS[op][1]; args=[self.var() for _ in range(n)]
            va=[]
            if op in VARARGS:
                cnt=self.var(); assert cnt[0]=='int'; va=[self.var() for _ in range(cnt[1])]
            ins.append((op,args,va))
        f['code']=ins; return f
    def obj(self):
        r=self.r; o={'name':self.s()}; size=r.u('I'); o['parent']=self.s(); o['doc']=self.s(); o['uflags']=r.u('I'); o['auto']=self.s()
        o['vars']=[]
        for _ in range(r.u('H')):
            n=self.s(); t=self.s(); r.u('I'); self.var(); o['vars'].append((n,t))
        o['props']=[]
        for _ in range(r.u('H')):
            n=self.s(); t=self.s(); self.s(); r.u('I'); fl=r.u('B'); p={'name':n,'type':t,'flags':fl,'funcs':[]}
            if fl&4: p['autovar']=self.s()
            else:
                if fl&1: p['funcs'].append(self.func('get'))
                if fl&2: p['funcs'].append(self.func('set'))
            o['props'].append(p)
        o['states']=[]
        for _ in range(r.u('H')):
            sn=self.s(); fs=[]
            for _ in range(r.u('H')):
                fn=self.s(); fs.append(self.func(fn))
            o['states'].append((sn,fs))
        return o
