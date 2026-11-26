#!/usr/bin/env python3
"""Run tests through a system Lua 5.4 interpreter or shared library."""
import ctypes as C, ctypes.util, pathlib, os, shutil, subprocess, sys
root = pathlib.Path(__file__).resolve().parents[1]
os.chdir(root)
modules = sorted(p.relative_to(root).as_posix()[:-4].replace('/','.') for p in (root/'std').rglob('*.lua'))
script = 'MODULES = {' + ','.join('"'+m+'"' for m in modules) + '}\n' + pathlib.Path('tests/test.lua').read_text()
exe = shutil.which('lua5.4') or shutil.which('lua54')
if exe:
    subprocess.run([exe,'-'],input=script,text=True,check=True)
else:
    name=ctypes.util.find_library('lua5.4')
    if not name: sys.exit('Lua 5.4 interpreter or shared library is required')
    lua=C.CDLL(name);lua.luaL_newstate.restype=C.c_void_p
    lua.luaL_openlibs.argtypes=[C.c_void_p];lua.luaL_loadstring.argtypes=[C.c_void_p,C.c_char_p]
    lua.lua_pcallk.argtypes=[C.c_void_p,C.c_int,C.c_int,C.c_int,C.c_longlong,C.c_void_p]
    lua.lua_tolstring.argtypes=[C.c_void_p,C.c_int,C.c_void_p];lua.lua_tolstring.restype=C.c_char_p
    lua.lua_close.argtypes=[C.c_void_p];L=lua.luaL_newstate();lua.luaL_openlibs(L)
    try:
        rc=lua.luaL_loadstring(L,script.encode()) or lua.lua_pcallk(L,0,0,0,0,None)
        if rc: sys.exit(lua.lua_tolstring(L,-1,None).decode())
    finally: lua.lua_close(L)
