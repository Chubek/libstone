"""Compile Lua modules and execute logistics with a deliberately small mock host.
This is not a test of the existing libstone ASDL parser or native implementation.
Requires Python 3 and a system Lua 5.4 shared library.
"""
import ctypes as C, ctypes.util, pathlib, re, os
root = pathlib.Path(__file__).resolve().parents[1]
os.chdir(root)
schema = (root/'examples/MiniRuby.asdl').read_text()
# Extract simple product/sum declarations in this example, not a general ASDL parser.
head = schema.split('/* Shared helpers')[0]
lines = ['local defs = {}']
for m in re.finditer(r'(?m)^([a-z_]+)\s*::=\s*(.*?);',head,re.S):
    ty, rhs = m.groups()
    for variant in rhs.split('|'):
        v = variant.strip()
        match = re.fullmatch(r'(?:(\w+)\s*)?\((.*?)\)',v,re.S)
        tag, fields = match.groups() if match else (v,'')
        entries=[]
        for f in fields.split(',') if fields.strip() else []:
            ft, name = f.strip().split()
            card = ft[-1] if ft[-1] in '?*+' else '1'
            entries.append('{name="%s",cardinality="%s"}'%(name,card))
        key=ty+':'+(tag or '')
        lines.append('defs["%s"]={%s}'%(key,','.join(entries)))
lines.append('return defs')
defs='\n'.join(lines)
blocks=re.findall(r'(?m)^\.logistics for :(\w+) ::= lua\(([^)]*)\)\n(.*?)^\.end',schema,re.S|re.M)
hooks='\n'.join('hooks["%s"] = function(%s)\n%s\nend'%(ty,args,body) for ty,args,body in blocks)
imports=[]
for names, lib in re.findall(r'\.import\s+([^;]+?)\s+from\s+([\w.]+);',schema):
    for name in names.split(','):
        n=name.strip(); imports.append('local %s = asdl.lib.require("%s").%s'%(n,lib,n))
script=(root/'tests/miniruby_mock.lua').read_text().replace('-- @DEFS@', 'local defs = (function()\n'+defs+'\nend)()').replace('-- @HOOKS@','\n'.join(imports)+'\n'+hooks)
libname=ctypes.util.find_library('lua5.4')
if not libname: raise SystemExit('Lua 5.4 shared library not found')
lua=C.CDLL(libname)
lua.luaL_newstate.restype=C.c_void_p
lua.luaL_openlibs.argtypes=[C.c_void_p]
lua.luaL_loadstring.argtypes=[C.c_void_p,C.c_char_p]
lua.lua_pcallk.argtypes=[C.c_void_p,C.c_int,C.c_int,C.c_int,C.c_longlong,C.c_void_p]
lua.lua_tolstring.argtypes=[C.c_void_p,C.c_int,C.c_void_p];lua.lua_tolstring.restype=C.c_char_p
lua.lua_close.argtypes=[C.c_void_p]
L=lua.luaL_newstate();lua.luaL_openlibs(L)
try:
    rc=lua.luaL_loadstring(L,script.encode()) or lua.lua_pcallk(L,0,0,0,0,None)
    if rc: raise SystemExit(lua.lua_tolstring(L,-1,None).decode())
    print(f'PASS: {len(blocks)} logistics compiled; mock semantic assertions passed.')
finally: lua.lua_close(L)
