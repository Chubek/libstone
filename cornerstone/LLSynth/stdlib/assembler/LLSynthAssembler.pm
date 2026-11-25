package main;

use strict;
use warnings;

sub python_assembler_interop {
 return <<'PY';
_assembler_sdk = None

def assembler_sdk_call(f, args):
    if len(args) != 3 or any(type(value) is not str or '\0' in value for value in args):
        raise LanguageError('INTEROP', 'assembler expects three NUL-free Strings')

    import ctypes
    import os
    import sys

    global _assembler_sdk
    try:
        source, selector, syntax = (value.encode('utf-8') for value in args)
        if _assembler_sdk is None:
            class Result(ctypes.Structure):
                _fields_ = [('bytes', ctypes.POINTER(ctypes.c_uint8)),
                            ('size', ctypes.c_size_t), ('owner', ctypes.c_void_p)]

            names = {'win32': 'llsynth_assembler.dll', 'darwin': 'libllsynth_assembler.dylib'}
            path = os.environ.get('LLSYNTH_ASSEMBLER_LIBRARY') or interop_library_path(
                names.get(sys.platform, 'libllsynth_assembler.so'))
            library = ctypes.CDLL(path)
            flat, release = library.llsynth_assembler_flat, library.llsynth_assembler_release
            flat.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int,
                             ctypes.POINTER(Result), ctypes.POINTER(ctypes.c_char), ctypes.c_size_t]
            flat.restype = ctypes.c_int
            release.argtypes = [ctypes.POINTER(Result)]
            release.restype = None
            _assembler_sdk = Result, flat, release
    except (OSError, AttributeError, UnicodeError) as exc:
        raise LanguageError('SANDSTONE', str(exc)) from None

    Result, flat, release = _assembler_sdk
    result, diagnostic = Result(), ctypes.create_string_buffer(2048)
    if flat(source, selector, syntax, int(f['assembler_selector'] == 'target'),
            ctypes.byref(result), diagnostic, len(diagnostic)):
        raise LanguageError('SANDSTONE', diagnostic.value.decode('utf-8', errors='replace')
                            or 'assembler failed')
    try:
        return list(ctypes.string_at(result.bytes, result.size)) if result.size else []
    finally:
        release(ctypes.byref(result))

PY
}

sub c_assembler_interop {
 return <<'C';
#include <llsynth_assembler.h>
static Val interop_assembler_sdk(Context *c,const char *source,const char *selector,const char *syntax,int use_target){
 Val nil=LLS_ZERO;
 Vec *out=new_vec(c);
 llsynth_assembler_result image;
 char diagnostic[2048];
 if(llsynth_assembler_flat(source,selector,syntax,use_target,&image,diagnostic,sizeof diagnostic)){
  error(c,"SANDSTONE: %s",diagnostic[0]?diagnostic:"assembler failed");
  return nil;
 }
 /* The SDK owns the borrowed image until release, including on allocator
    failure in the generated runtime. */
 jmp_buf handler;
 jmp_buf *previous=c->oom;
 c->oom=&handler;
 if(setjmp(handler)){
  llsynth_assembler_release(&image);
  c->oom=previous;
  if(previous)longjmp(*previous,1);
  abort();
 }
 if(image.size){
  out->a=(Val*)ca(c,image.size*sizeof(Val));
  out->cap=out->n=(int)image.size;
  for(size_t i=0;i<image.size;i++)out->a[i]=vint(image.bytes[i]);
 }
 llsynth_assembler_release(&image);
 c->oom=previous;
 return vvec(out);
 }
C
}

1;
