package main;

use strict;
use warnings;
use utf8;
use File::Basename qw(dirname);
use File::Path qw(make_path);
use File::Spec ();
use MIME::Base64 qw(encode_base64);
use Encode ();

sub has_ipc {scalar grep {($_->{foreign}||'') eq 'ipc'} @{$_[0]->{functions}}}
sub ipc_runtime_path {File::Spec->catfile(dirname(__FILE__),'..','stdlib','ipc',$_[0] eq 'python3'?'runtime.py':'runtime.c')}

sub generate_python {
 my($s,$o,$json)=@_;
 for my $flag(qw(header runtime semant rewriter table lexer parser_out ast ir driver)){
  defined $o->{$flag} and fail("Python output does not support --$flag",$s->{file},1);
 }
 $o->{split} =~ /^(none|explicit)$/ or fail('Python output supports split none or explicit',$s->{file},1);
 my $file=outpath($o->{output} || lc($s->{language}).'.py');
 my $encoded=encode_base64($json->encode($s),'');
  my $runtime=template('python');
  $runtime =~ s/\@\@INTEROP_PYTHON\@\@/python_interop($s)/ge;
  my $action_semantics=$o->{action_semantics_artifact}?${$o->{action_semantics_artifact}}:'';
  $runtime =~ s/\@\@ACTION_SEMANTICS_PYTHON\@\@/$action_semantics/g;
 $runtime =~ s/\@\@SPEC_BASE64\@\@/$encoded/g;
 $runtime =~ s/\@\@MODE\@\@/$o->{mode}/g;
 $runtime =~ s/\@\@TARGET\@\@/$o->{program_target}/g;
 $runtime =~ s/\nif __name__ == '__main__':\n    sys.exit\(main\(\)\)\s*$/\n/ if $o->{driver_mode} eq 'library';
 my %files;
 if($o->{package}){
  defined $o->{output} and die "use either --package or --output for Python generation\n";
  $o->{package}=~/^[A-Za-z_]\w*$/ or fail('invalid Python package name',$s->{file},1);
  -d $o->{out_dir} or die "output directory does not exist: $o->{out_dir}\n";
  make_path(outpath($o->{package}));
  $file=outpath($o->{package}.'/__init__.py');
  $files{outpath($o->{package}.'/__main__.py')}="from . import main\nraise SystemExit(main())\n" unless $o->{driver_mode} eq 'library';
 }
 $files{$file}=$runtime;
 return write_outputs(\%files,$file);
}

sub public_header {
 my($base,$mode,$result_type)=@_;$result_type||='Float64';
 my %ctypes=(Float64=>'double',Int=>'int64_t',Bool=>'int',String=>'const char *',Void=>'double');my $ctype=$ctypes{$result_type}||'double';
 my $h=<<'HEADER';
#ifndef LLS_PUBLIC_HEADER
#define LLS_PUBLIC_HEADER
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct LLS_Context LLS_Context;
typedef struct LLS_Program LLS_Program;
typedef struct LLS_Diagnostic {
    char message[512];
    char rule_stack[512];
    int line, column;
} LLS_Diagnostic;
typedef enum {
    LLS_OK = 0, LLS_SYNTAX_ERROR, LLS_SEMANTIC_ERROR,
    LLS_RUNTIME_ERROR, LLS_OOM
} LLS_Status;
LLS_Context *LLS_context_new(void);
void LLS_context_free(LLS_Context *);
void LLS_context_reset(LLS_Context *);
void LLS_diagnostic_free(LLS_Diagnostic *);
const char *LLS_program_data(const LLS_Program *);
size_t LLS_program_size(const LLS_Program *);
void LLS_program_free(LLS_Program *);
HEADER
 $h.="LLS_Status LLS_interpret(LLS_Context *, const char *, size_t, $ctype *, LLS_Diagnostic **);\n" if $mode =~ /^(both|interpret)$/;
 $h.="LLS_Status LLS_compile(LLS_Context *, const char *, size_t, LLS_Program **, LLS_Diagnostic **);\n" if $mode =~ /^(both|compile|rewrite)$/;
 $h.="LLS_Status LLS_analyze(LLS_Context *, const char *, size_t, LLS_Diagnostic **);\n" if $mode eq 'analyze';
 $h.=<<'CPP';
#ifdef __cplusplus
}
#include <memory>
#include <new>
struct LLS_ContextDeleter {void operator()(LLS_Context *p) const noexcept {LLS_context_free(p);}};
struct LLS_ProgramDeleter {void operator()(LLS_Program *p) const noexcept {LLS_program_free(p);}};
struct LLS_DiagnosticDeleter {void operator()(LLS_Diagnostic *p) const noexcept {LLS_diagnostic_free(p);}};
using LLS_ProgramOwner = std::unique_ptr<LLS_Program, LLS_ProgramDeleter>;
using LLS_DiagnosticOwner = std::unique_ptr<LLS_Diagnostic, LLS_DiagnosticDeleter>;
class LLS_ContextOwner {
 std::unique_ptr<LLS_Context, LLS_ContextDeleter> handle_;
public:
 LLS_ContextOwner():handle_(LLS_context_new()){if(!handle_)throw std::bad_alloc();}
 LLS_Context *get() const noexcept {return handle_.get();}
 void reset(){LLS_context_reset(handle_.get());}
};
#endif
#endif
CPP

 $h=~s/LLS_PUBLIC_HEADER/uc($base).'_GENERATED_H'/ge;
 $h=~s/\bLLS_/${base}_/g;
 return $h;
}
sub public_api {
 my($base,$mode,$result_type)=@_;$result_type||='Float64';
 my %ctypes=(Float64=>'double',Int=>'int64_t',Bool=>'int',String=>'const char *',Void=>'double');my $ctype=$ctypes{$result_type}||'double';
 my $code=<<'API';
struct LLS_Context {Context engine;};
struct LLS_Program {char *data;size_t size;};
LLS_Context *LLS_context_new(void){
 LLS_Context *c=(LLS_Context*)calloc(1,sizeof(LLS_Context));
 if(!c)return NULL;
 jmp_buf handler;c->engine.oom=&handler;
 if(setjmp(handler)){clear_ctx(&c->engine);free(c);return NULL;}
 reset(&c->engine);c->engine.oom=NULL;return c;
}
void LLS_context_free(LLS_Context *c){if(c){clear_ctx(&c->engine);free(c);}}
void LLS_context_reset(LLS_Context *c){if(!c)return;jmp_buf handler;c->engine.oom=&handler;
 if(setjmp(handler)){clear_ctx(&c->engine);snprintf(c->engine.error,sizeof(c->engine.error),"OOM: context reset failed");}
 else reset(&c->engine);
 c->engine.oom=NULL;
}
void LLS_diagnostic_free(LLS_Diagnostic *d){free(d);}
const char *LLS_program_data(const LLS_Program *p){return p?p->data:NULL;}
size_t LLS_program_size(const LLS_Program *p){return p?p->size:0;}
void LLS_program_free(LLS_Program *p){if(p){free(p->data);free(p);}}
static LLS_Status api_diagnostic(Context *c,LLS_Diagnostic **out){
 if(out){*out=(LLS_Diagnostic*)calloc(1,sizeof(LLS_Diagnostic));
  if(!*out)return LLS_OOM;
  snprintf((*out)->message,sizeof((*out)->message),"%s",c->error);
  snprintf((*out)->rule_stack,sizeof((*out)->rule_stack),"%s",c->rule_stack);
  (*out)->line=c->line;(*out)->column=c->col;
 }
 if(!strncmp(c->error,"LEX:",4)||!strncmp(c->error,"PARSE:",6))return LLS_SYNTAX_ERROR;
 return LLS_SEMANTIC_ERROR;
}
static char *api_source(Context *c,const char *source,size_t size){
 if(!source&&size){error(c,"LEX: null source");return NULL;}
 if(size==(size_t)-1){error(c,"LEX: source too large");return NULL;}
 if(size&&memchr(source,0,size)){error(c,"LEX: embedded NUL is not supported");return NULL;}
 char *copy=(char*)ca(c,size+1);
 if(!copy){error(c,"out of memory");return NULL;}
 if(size)memcpy(copy,source,size);
 copy[size]=0;return copy;
}
API
 if($mode =~ /^(both|interpret)$/){$code.=<<'API';
static LLS_Status api_interpret(LLS_Context *ctx,const char *source,size_t size,LLS_RESULT_TYPE *result,LLS_Diagnostic **diag){
 if(diag)*diag=NULL;
 if(result)*result=0;
 if(!ctx)return LLS_RUNTIME_ERROR;
 Context *c=&ctx->engine;reset(c);
 char *copy=api_source(c,source,size);
 if(!copy)return api_diagnostic(c,diag);
 Val value=run(c,copy,"semantics");
 if(!c->error[0]&&!(LLS_RESULT_VALID))error(c,"SEM: result does not match the public API type");
 if(c->error[0])return api_diagnostic(c,diag);
 if(result)*result=LLS_RESULT_VALUE;
 return LLS_OK;
}
API
 }
 if($mode =~ /^(both|compile|rewrite)$/){$code.=<<'API';
static LLS_Status api_compile(LLS_Context *ctx,const char *source,size_t size,LLS_Program **result,LLS_Diagnostic **diag){
 if(diag)*diag=NULL;
 if(result)*result=NULL;
 if(!ctx||!result)return LLS_RUNTIME_ERROR;
 Context *c=&ctx->engine;reset(c);
 char *copy=api_source(c,source,size);
 if(!copy)return api_diagnostic(c,diag);
 Val value=run(c,copy,"lower");
 Builder b=LLS_ZERO;b.ctx=c;
 if(!c->error[0]){
  Vec *ops=value.tag==VVEC?value.vec:map_get(c->states,"code").vec;
  if(!ops)error(c,"BACKEND: missing IR code vector");
  else {rewrite_ops(c,ops);if(!c->error[0])compile_output(c,&b,LLS_TARGET,ops);}
 }
 if(c->error[0])return api_diagnostic(c,diag);
 LLS_Program *p=(LLS_Program*)calloc(1,sizeof(LLS_Program));
 if(!p)return LLS_OOM;
 p->data=(char*)malloc(b.n+1);if(!p->data){free(p);return LLS_OOM;}
 if(b.n)memcpy(p->data,b.data,b.n);
 p->data[b.n]=0;p->size=b.n;*result=p;return LLS_OK;
}
API
 }
 if($mode eq 'analyze'){$code.=<<'API';
LLS_Status LLS_analyze(LLS_Context *ctx,const char *source,size_t size,LLS_Diagnostic **diag){
 if(diag)*diag=NULL;
 if(!ctx)return LLS_RUNTIME_ERROR;
 jmp_buf handler;ctx->engine.oom=&handler;
 if(setjmp(handler)){clear_ctx(&ctx->engine);ctx->engine.oom=NULL;return LLS_OOM;}
 Context *c=&ctx->engine;reset(c);char *copy=api_source(c,source,size);
 if(copy)(void)run(c,copy,"analyze");
 LLS_Status status=c->error[0]?api_diagnostic(c,diag):LLS_OK;ctx->engine.oom=NULL;return status;
}
API
 }
 for my $entry(qw(interpret compile)){
  next unless $mode eq 'both' || $mode eq $entry || ($entry eq 'compile' && $mode eq 'rewrite');
  my $pointer_type=$entry eq 'interpret'?"$ctype *":'LLS_Program **';
  $code.="LLS_Status LLS_${entry}(LLS_Context *ctx,const char *source,size_t size,$pointer_type result,LLS_Diagnostic **diag){\n";
  $code.=" if(diag)*diag=NULL;\n if(result)*result=0;\n if(!ctx)return LLS_RUNTIME_ERROR;\n jmp_buf handler;ctx->engine.oom=&handler;\n";
  $code.=" if(setjmp(handler)){clear_ctx(&ctx->engine);ctx->engine.oom=NULL;return LLS_OOM;}\n";
  $code.=" LLS_Status status=api_${entry}(ctx,source,size,result,diag);ctx->engine.oom=NULL;return status;\n}\n";
 }
 my %valid=(Float64=>'value.tag==VNUM||value.tag==VINT',Int=>'value.tag==VINT',Bool=>'value.tag==VBOOL',String=>'value.tag==VSTR',Void=>'value.tag==VNIL');
 my %value=(Float64=>'number(value)',Int=>'value.i',Bool=>'truth(value)',String=>'value.s',Void=>'0');
 $code=~s/LLS_RESULT_TYPE/$ctype/g;$code=~s/LLS_RESULT_VALID/$valid{$result_type}/g;$code=~s/LLS_RESULT_VALUE/$value{$result_type}/g;
 $code =~ s/\bLLS_(?!TARGET|ZERO)/${base}_/g;
 return $code;
}
sub generate_c {
 my($s,$o,$json)=@_;
 my $base=$o->{module_prefix}||$s->{language};$base =~ s/[^A-Za-z0-9_]/_/g;
 $base='L_'.$base if $base =~ /^\d/;
 my $suffix=$o->{T}=~/^cpp/ ? '.cpp' : '.c';
 if($o->{split} =~ /^(facets|all)$/){
  $o->{header}||="$base.h";
  for my $facet(qw(runtime semant rewriter lexer parser_out ir driver ast)){
   next if $facet eq 'driver' && $o->{driver_mode} eq 'library';
   $o->{$facet}||=$base.'_'.$facet.$suffix;
  }
  $o->{table}||="${base}Tables.i";
  if($o->{split} eq 'all'){$o->{$_}||=$base.'_'.$_.$suffix for qw(json values memory numeric io api)}
 }
  my $main=outpath($o->{output}||"$base$suffix");
  my %paths=(output=>$main);
  if($o->{action_semantics_artifact}){
   (my $stem=$main)=~s/\Q$suffix\E$//;
   $paths{action_semantics}=$stem.'.actionsemantics.c';
  }
 for my $facet(qw(header runtime semant rewriter table lexer parser_out ast ir driver json values memory numeric io api manifest depfile)){
  next unless defined $o->{$facet};
  next if $o->{split} eq 'none' && $facet !~ /^(header|manifest|depfile)$/;
  $paths{$facet}=outpath($o->{$facet});
 }
 my %seen;
 my $spec_path=canonical_path($s->{file});
 for my $facet(sort keys %paths){
  my $abs=canonical_path($paths{$facet});
  die "output path collision: $paths{$facet} ($facet and $seen{$abs})\n" if $seen{$abs};
  $seen{$abs}=$facet;
  $abs eq $spec_path and die "output would overwrite specification: $paths{$facet}\n";
   if($facet !~ /^(header|table|manifest|depfile|action_semantics)$/){
   $paths{$facet} =~ /\Q$suffix\E$/ or die "incompatible $o->{T} output suffix: $paths{$facet}\n";
  }
 }
 my $constant="const char lls_spec_json[] = {\n";
 my @byte=unpack('C*',$json->encode($s));
 # Non-ASCII UTF-8 bytes otherwise narrow when char is signed in C++.
 while(@byte){my @row=splice(@byte,0,32);$constant.='  '.join(', ',map {$_<128?$_:"(char)$_"} @row).",\n"}
 $constant.="  0\n};\n";
  my $code=template('c');
  $code =~ s/\@\@INTEROP_C\@\@/c_interop($s)/ge;
  my $action_entry="${base}_impl_action_semantics_entry";
  my $action_adapter=$paths{action_semantics}?LLSynthActionSemantics::c_adapter($action_entry):'';
  my $ordinary_call='exec_block(c,jo(jo(rule,"code"),facet),env,caps,&r);';
  my $action_call=$paths{action_semantics}?<<'C':$ordinary_call;
Json *action=jo(rule,"action_semantics");
     if(action && !strcmp(facet,"semantics"))r=acsem_eval(c,action,caps);
     else exec_block(c,jo(jo(rule,"code"),facet),env,caps,&r);
C
  $action_call.='     ' if $paths{action_semantics};
  $code =~ s/\@\@ACTION_SEMANTICS_C\@\@/$action_adapter/g;
  $code =~ s/\@\@ACTION_SEMANTICS_CALL_C\@\@/$action_call/g;
 $code =~ s/\@\@MODE\@\@/$o->{mode}/g;
 $code =~ s/\@\@TARGET\@\@/$o->{program_target}/g;
 my($start_rule)=grep {$_->{name} eq $s->{start}} @{$s->{rules}};
 my $result_type=$start_rule->{return_types}{semantics}||'Void';
 if($o->{mode}=~/^(interpret|both)$/ && $result_type!~/^(Float64|Int|Bool|String|Void)$/){fail("C embedding currently requires a scalar start-rule result, got $result_type",$s->{file},1)}
 my $header=public_header($base,$o->{mode},$result_type);
 my $api=public_api($base,$o->{mode},$result_type);
 my $driver=$o->{driver_mode} eq 'library'?'':"int main(int argc,char **argv){return llsynth_generated_main(argc,argv);}\n";
 # Split the runtime into actual translation units. A generated private header
 # carries shared layouts and prototypes, never function bodies or data.
 my($declarations,$functions)=split_c_runtime($code.$api);
 my %chunks;
 for my $f(@$functions){my($name,$body)=@$f;
  my $facet=$name =~ /^(lex|valid_utf8)$/?'lexer':$name eq 'parse'?'parser_out':
   $name =~ /^(eval|eval_call|exec_block|walk|walk_node|run|reset)$/?'semant':
   $name =~ /^(rewrite_ops|same_ops|is_identifier|op_effect)$/?'rewriter':
    $name =~ /^(compile_output|compile_vmgen|render_template|verify_ops|bytecode_id|encode_bytecode|execute_bytecode)$/?'ir':
   $name =~ /^(new_node|first_tok)$/?'ast':
   $name eq 'llsynth_generated_main'?'driver':'runtime';
  if($o->{split} eq 'all' && $facet eq 'runtime'){
   $facet=$name =~ /^(jws|jhex|jstring|jparse|jo|ji|js|jg|jlen|jnumber|jeq|lls_spec)$/?'json':
    $name =~ /^(ca|crealloc|clear_ctx|allocation_failure|dup_n)$/?'memory':
    $name =~ /^(ascii_strtod|ascii_format|number_literal|int_binary|quoted_string)$/?'numeric':
    $name =~ /^(build|read_source)$/?'io':
    $name =~ /^(api_|\Q${base}_\E)/?'api':'values';
  }
  $chunks{$facet}.=slurp(ipc_runtime_path('c11')) if $name eq 'interop_call' && has_ipc($s);
  $chunks{$facet}.=$body;
 }
 my %files;
 my $split=grep {exists $paths{$_}} qw(runtime semant rewriter lexer parser_out ir driver ast);
 my $private=File::Spec->catfile(dirname($main),$base.'_internal.h');
 die "output path collision: $private\n" if $split && $seen{canonical_path($private)};
 my $pre_ipc=has_ipc($s)?"#ifndef LLS_IPC_DISABLE\n#ifndef _GNU_SOURCE\n#define _GNU_SOURCE\n#endif\n#ifndef _POSIX_C_SOURCE\n#define _POSIX_C_SOURCE 200809L\n#endif\n#endif\n":'';
 my $preamble=$pre_ipc.$header.$declarations;
 if($split){$files{$private}=$preamble;}
 my $prefix=sub {my($owner)=@_;return $split?'#include '.relative_include($private,$owner)."\n":$preamble};
 $files{$main}=$prefix->($main);
 for my $facet(sort keys %chunks){
  my $owner=$paths{$facet}||$main;
  $files{$owner}//=$prefix->($owner);
  $files{$owner}.=$chunks{$facet};
 }
 my $owner=$paths{runtime}||$main;
 $files{$owner}//=$prefix->($owner);
 if($paths{table}){
  if($paths{table}=~/\.(?:i|inc)$/){
   $files{$paths{table}}=$constant;
   $files{$owner}.='#include '.relative_include($paths{table},$owner)."\n";
  }else{
   $paths{table}=~/\Q$suffix\E$/ or die "incompatible table output suffix: $paths{table}\n";
   $files{$paths{table}}="#ifdef __cplusplus\nextern \"C\" const char lls_spec_json[];\n#else\nextern const char lls_spec_json[];\n#endif\n".$constant;
  }
 }else{$files{$owner}.=$constant}
 $owner=$paths{driver}||$main;
 $files{$owner}//=$prefix->($owner);
 $files{$owner}.=$driver;
 $files{$paths{header}}=$header if $paths{header};
 # Every non-static implementation symbol is private to this language prefix.
 my @names=('lls_spec_json',map {$_->[0]} @$functions);
 my %renames=map {$_=>"${base}_impl_$_"} grep {!/^\Q${base}_\E/} @names;
 # Restore external symbols only after internal names have been prefixed.
 my %native_symbols=map {("__lls_native_symbol_$_"=>$s->{functions}[$_]{symbol})}
  grep {($s->{functions}[$_]{foreign}||'') eq 'c'} 0..$#{$s->{functions}};
 for my $path(keys %files){
  my $public=index($files{$path},$header)==0?substr($files{$path},0,length($header),''):'';
  $files{$path}=~s{(/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|\b[A-Za-z_]\w*\b)}{exists $renames{$1}?$renames{$1}:$1}gse;
  $files{$path}=$public.$files{$path};
   $files{$path}=~s/\b(__lls_native_symbol_\d+)\b/exists $native_symbols{$1}?$native_symbols{$1}:$1/ge;
  }
  if($paths{action_semantics}){
   my $source=${$o->{action_semantics_artifact}};
   $source =~ s/^int LLS_ACTION_SEMANTICS_ENTRY\(/int $action_entry(/m;
   $files{$paths{action_semantics}}=$source;
  }
  return write_outputs(\%files,$main);
}

# Interop stays opt-in: Python imports ctypes/subprocess only on first use;
# C emits native references and the Sandstone SDK include only when declared.
sub python_interop {
 my($s)=@_;
 my $sdk=grep {($_->{foreign}||'') eq 'sandstone_sdk'} @{$s->{functions}};
 my $runtime=<<'PY';
_interop_libraries = {}
_interop_functions = {}

def interop_value(kind, value):
    valid = ((kind == 'Int' and type(value) is int and -(1 << 63) <= value < (1 << 63)) or
             (kind == 'Float64' and type(value) in (int, float)) or
             (kind == 'Bool' and type(value) is bool) or
             (kind == 'String' and type(value) is str and '\0' not in value))
    if not valid: raise LanguageError('INTEROP', f'expected native {kind}, got {type(value).__name__}')
    return value.encode('utf-8') if kind == 'String' else value

def interop_library_path(name):
    import os
    if not os.path.dirname(name):
        for directory in os.environ.get('LLSYNTH_LIBRARY_PATH', '').split(':'):
            if directory:
                path = os.path.abspath(os.path.join(directory, name))
                if os.path.isfile(path):
                    return path
    return name

def interop_call(f, args):
    if f['foreign'] == 'sandstone':
        if len(args) != 1: raise LanguageError('INTEROP', 'Sandstone expects one source String')
        source = interop_value('String', args[0])
        import subprocess
        import tempfile
        import os
        try:
            with tempfile.TemporaryDirectory(prefix='llsynth-asm-') as directory:
                input_path, output_path = os.path.join(directory, 'input.s'), os.path.join(directory, 'output.bin')
                with open(input_path, 'wb') as stream: stream.write(source)
                command = [f['cli'], '--bank', f['bank'], '-f', 'bin', '-o', output_path]
                if f['syntax']: command += ['--syntax', f['syntax']]
                result = subprocess.run(command + [input_path], capture_output=True, timeout=30, check=False)
                if result.returncode:
                    raise LanguageError('SANDSTONE', result.stderr.decode('utf-8', errors='replace').strip() or
                                        f'assembler exited with status {result.returncode}')
                with open(output_path, 'rb') as stream: data = stream.read(64 * 1024 * 1024 + 1)
                if len(data) > 64 * 1024 * 1024: raise LanguageError('SANDSTONE', 'flat image exceeds 64 MiB')
                return list(data)
        except (OSError, subprocess.SubprocessError) as exc:
            raise LanguageError('SANDSTONE', f'cannot assemble using {f["cli"]}: {exc}') from None
    params = f['native_params']
    if len(params) != len(args): raise LanguageError('INTEROP', f'{f["name"]} expects {len(params)} arguments')
    values = [interop_value(t, v) for t, v in zip(params, args)]
    import ctypes
    types = {'Int': ctypes.c_int64, 'Float64': ctypes.c_double, 'Bool': ctypes.c_int,
             'String': ctypes.c_char_p, 'Void': None}
    key = (f['library'], f['symbol'], tuple(params), f['native_return'])
    try:
        if key not in _interop_functions:
            if f['library'] not in _interop_libraries:
                _interop_libraries[f['library']] = ctypes.CDLL(interop_library_path(f['library']))
            # Attribute access caches ctypes objects; indexing gives each signature
            # its own object without mutating another declaration's argtypes.
            fn = _interop_libraries[f['library']][f['symbol']]
            fn.argtypes, fn.restype = [types[t] for t in params], types[f['native_return']]
            _interop_functions[key] = fn
        value = _interop_functions[key](*values)
        kind = f['native_return']
        if kind == 'String':
            if value is None: raise LanguageError('INTEROP', f'{f["name"]} returned a null String')
            return value.decode('utf-8')
        return bool(value) if kind == 'Bool' else value
    except (OSError, AttributeError, TypeError, ValueError, OverflowError, UnicodeError, ctypes.ArgumentError) as exc:
        raise LanguageError('INTEROP', f'{f["name"]}: {exc}') from None
PY
 my $dispatch='';
 $dispatch.="    if f['foreign'] == 'sandstone_sdk': return assembler_sdk_call(f, args)\n" if $sdk;
 $dispatch.="    if f['foreign'] == 'ipc': return ipc_call(f, args)\n" if has_ipc($s);
 $runtime =~ s/(def interop_call\(f, args\):\n)/$1$dispatch/;
 $runtime=python_assembler_interop().$runtime if $sdk;
 $runtime.=slurp(ipc_runtime_path('python3')) if has_ipc($s);
 return $runtime;
}

sub c_interop {
 my($s)=@_;my $code='';
 my @foreign=grep {$_->{foreign}} @{$s->{functions}};
 $code.=c_assembler_interop() if grep {$_->{foreign} eq 'sandstone_sdk'} @foreign;
 if(grep {$_->{foreign} eq 'sandstone'} @foreign){$code.=<<'C';
#include <assembler.h>
static Val interop_assemble(Context *c,const char *source,const char *bank,const char *syntax){
 Val nil=LLS_ZERO;Vec *out=new_vec(c);sandstone_asm_t *as=NULL;char diagnostic[512]={0};
 if(sandstone_asm_new(&as,bank,diagnostic,sizeof diagnostic)){
  error(c,"SANDSTONE: %s",diagnostic[0]?diagnostic:"cannot create assembler");return nil;
 }
 if(sandstone_asm_set_syntax(as,syntax)||sandstone_asm_assemble_string(as,source,"<llsynth>")){
  error(c,"SANDSTONE: %s",sandstone_asm_error(as));sandstone_asm_free(as);return nil;
 }
 uint32_t relocs=UINT32_MAX;sandstone_asm_relocs(as,&relocs);
 if(relocs){error(c,"SANDSTONE: flat output has unresolved relocations; linking/object output is unsupported");sandstone_asm_free(as);return nil;}
 size_t size=0;const uint8_t *bytes=sandstone_asm_image(as,&size);
 if(!bytes){for(uint32_t i=0;i<sandstone_asm_section_count(as);i++){
  size_t section_size=0;sandstone_asm_section_bytes(as,i,&section_size);
  if(section_size){error(c,"SANDSTONE: cannot obtain flat image");sandstone_asm_free(as);return nil;}
 }}
 if((!bytes&&size)||size>64*1024*1024){error(c,"SANDSTONE: cannot obtain flat image");sandstone_asm_free(as);return nil;}
 /* Keep the SDK context alive while copying its borrowed bytes, and release it
    even if the generated allocator unwinds through its OOM recovery point. */
 jmp_buf handler;jmp_buf *previous=c->oom;c->oom=&handler;
 if(setjmp(handler)){sandstone_asm_free(as);c->oom=previous;if(previous)longjmp(*previous,1);abort();}
 if(size){out->a=(Val*)ca(c,size*sizeof(Val));out->cap=(int)size;out->n=(int)size;
  for(size_t i=0;i<size;i++)out->a[i]=vint(bytes[i]);}
 sandstone_asm_free(as);c->oom=previous;return vvec(out);
}
C
 }
 my %ct=(Int=>'int64_t',Float64=>'double',Bool=>'int',String=>'const char *',Void=>'void');
 for my $i(0..$#{$s->{functions}}){my $f=$s->{functions}[$i];next unless ($f->{foreign}||'') eq 'c';
  $code.='extern '.$ct{$f->{native_return}}." __lls_native_symbol_$i(".join(', ',map {$ct{$_}} @{$f->{native_params}}).(@{$f->{native_params}}?'':'void').");\n";
 }
  $code.="static Val interop_call(Context *c,Json *f,Vec *args){Val nil=LLS_ZERO;const char *name=jg(f,\"name\");(void)args;(void)name;\n";
  $code.=' if(!strcmp(jg(f,"foreign"),"ipc"))return lls_ipc_call(c,jg(f,"symbol"),args);'."\n" if has_ipc($s);
 my %valid=(Int=>'v.tag==VINT',Float64=>'(v.tag==VINT||v.tag==VNUM)',Bool=>'v.tag==VBOOL',String=>'v.tag==VSTR');
 my %get=(Int=>'args->a[INDEX].i',Float64=>'number(args->a[INDEX])',Bool=>'truth(args->a[INDEX])',String=>'args->a[INDEX].s');
  for my $i(0..$#{$s->{functions}}){my $f=$s->{functions}[$i];next unless $f->{foreign};
   next if $f->{foreign} eq 'ipc';
  my @params=$f->{foreign} eq 'c'?@{$f->{native_params}}:
             $f->{foreign} eq 'sandstone_sdk'?('String') x 3:('String');
  $code.=' if(!strcmp(name,"'.$f->{name}.'")){'."\n";
  $code.='  if(args->n!='.scalar(@params).'){error(c,"INTEROP: '.$f->{name}.' argument count mismatch");return nil;}'."\n";
  for my $j(0..$#params){my $test=$valid{$params[$j]};$test=~s/\bv\b/args->a[$j]/g;
   $code.="  if(!($test)){error(c,\"INTEROP: $f->{name} expected native $params[$j]\");return nil;}\n";
  }
  if($f->{foreign} eq 'sandstone'){
   my($bank,$syntax)=map {c_interop_string($_)} ($f->{bank},$f->{syntax});
   $code.="  return interop_assemble(c,args->a[0].s,$bank,$syntax);\n";
  }elsif($f->{foreign} eq 'sandstone_sdk'){
   my $use_target=$f->{assembler_selector} eq 'target'?1:0;
   $code.="  return interop_assembler_sdk(c,args->a[0].s,args->a[1].s,args->a[2].s,$use_target);\n";
  }else{
   my @args;for my $j(0..$#params){my $value=$get{$params[$j]};$value=~s/INDEX/$j/g;push @args,$value;}
   my $call="__lls_native_symbol_$i(".join(',',@args).')';my $ret=$f->{native_return};
   if($ret eq 'Void'){$code.="  $call;return nil;\n"}
   elsif($ret eq 'String'){$code.="  const char *value=$call;if(!value){error(c,\"INTEROP: $f->{name} returned a null String\");return nil;}if(!valid_utf8((const unsigned char*)value)){error(c,\"INTEROP: invalid native UTF-8 String\");return nil;}return vstr(dup_n(c,value,strlen(value)));\n"}
   else {my %wrap=(Int=>'vint',Float64=>'vnum',Bool=>'vbool');$code.="  return $wrap{$ret}($call);\n"}
  }
  $code.=" }\n";
 }
 $code.=" error(c,\"INTEROP: unknown foreign function %s\",name);return nil;\n}\n";
 return $code;
}
sub c_interop_string {
 my($s)=@_;return '"'.join('',map {sprintf('\\%03o',$_)} unpack('C*',Encode::encode('UTF-8',$s))).'"';
}

# The embedded C runtime uses top-level declarations and function definitions.
# Tokenizing strings/comments lets us separate bodies without interpreting C.
sub split_c_runtime {
 my($source)=@_;my($decl,@functions);$decl='';
 my @prototypes;
 # Preprocessor linkage wrappers are unnecessary: the shared declarations
 # establish C linkage for both the public and private functions.

 $source =~ s/#ifdef __cplusplus\nextern "C"\n#endif\n//g;
 my $start=0;my $depth=0;my $function;
 while($source =~ m{(/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[{};])}gs){
  my $token=$1;my $end=pos($source);next if length($token)>1;
  if($token eq '{'){
   if(!$depth){my $prefix=substr($source,$start,$end-$start-1);
    if($prefix =~ /((?:static\s+)?(?:const\s+)?[A-Za-z_]\w*(?:\s*\*)?\s+\**(\w+)\([^;{}]*\))\s*$/s){
     my($sig,$name)=($1,$2);my $offset=length($prefix)-length($&);
     $decl.=substr($prefix,0,$offset);
     $sig =~ s/^static\s+//;
     $function=[$name,$sig];push @prototypes,$sig.';';$start=$end;
    }
   }
   $depth++;
  }elsif($token eq '}'){
   $depth--;
   if(!$depth && $function){push @functions,[$function->[0],$function->[1].'{'.substr($source,$start,$end-$start)."\n"];$function=undef;$start=$end;}
  }elsif(!$depth && $token eq ';'){
   my $part=substr($source,$start,$end-$start);
   $part =~ s/\bstatic\s+//g;
   $decl.=$part;$start=$end;
  }
 }
 $decl.=substr($source,$start);
 $decl.="\n".join("\n",@prototypes)."\n";
 return ($decl,\@functions);
}

1;
