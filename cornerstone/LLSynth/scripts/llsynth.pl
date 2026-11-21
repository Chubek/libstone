#!/usr/bin/env perl
use strict;
use warnings;
use utf8;
use FindBin qw($RealBin);
use Getopt::Long qw(GetOptions);
use JSON::PP ();
use File::Path qw(make_path);
use File::Basename qw(dirname basename);
use File::Spec;
use Cwd ();
use File::Temp qw(tempfile);
use Math::BigInt;
use Encode ();
use POSIX ();
use Digest::SHA qw(sha256_hex);
our $VERSION = '0.7.0';
our %opt=(T=>'python3', split=>'explicit', diagnostics=>'text', parser=>'', program_target=>'c99', out_dir=>'.', driver_mode=>'cli');
our @include_paths;
our @module_paths;
my @generator_modules=map {"$RealBin/$_"} qw(LLSynthCompiler.pm LLSynthEmit.pm LLSynthMeta.pm LLSynthModules.pm LLSynthCompanions.pm);
require $_ for @generator_modules;
my (@extensions, @extension_paths);

GetOptions('T|tool-target=s'=>\$opt{T},'o|output=s'=>\$opt{output},'h|header=s'=>\$opt{header},
  'mode=s'=>\$opt{mode},'program-target=s'=>\$opt{program_target},'parser=s'=>\$opt{parser},
  'split=s'=>\$opt{split},'out-dir=s'=>\$opt{out_dir},'package=s'=>\$opt{package},
  'runtime=s'=>\$opt{runtime},'semant=s'=>\$opt{semant},'rewriter=s'=>\$opt{rewriter},
  'table=s'=>\$opt{table},'lexer=s'=>\$opt{lexer},'parser-out=s'=>\$opt{parser_out},
  'ast=s'=>\$opt{ast},'ir=s'=>\$opt{ir},'driver=s'=>\$opt{driver},'manifest=s'=>\$opt{manifest},
  'check'=>\$opt{check},'help'=>\$opt{help},'version'=>\$opt{version},'diagnostics=s'=>\$opt{diagnostics},
  'dump-grammar:s'=>\$opt{dump_grammar},'dump-tables:s'=>\$opt{dump_tables},
  'warnings-as-errors'=>\$opt{warnings_as_errors}, 'include-path=s@'=>\@include_paths, 'module-path=s@'=>\@module_paths,
  'module-prefix=s'=>\$opt{module_prefix},'driver-mode=s'=>\$opt{driver_mode},'deterministic'=>\$opt{deterministic},
  'trace-parser'=>\$opt{trace_parser},'max-errors=i'=>\$opt{max_errors},
  'list-extensions'=>\$opt{list_extensions},'extension=s@'=>\@extensions,'extension-path=s@'=>\@extension_paths,
  'depfile=s'=>\$opt{depfile},'emit-table-format=s'=>\$opt{table_format}) or usage(2);
if($opt{help}){usage(0)}
if($opt{version}){print "llsynth.pl $VERSION\n"; exit 0}
($opt{list_extensions} ? !@ARGV && !@extensions : @ARGV==1) or usage(2);
my $input=$ARGV[0] // '<command line>';
$opt{mode} //= @extensions ? 'analyze' : 'both';
my $json=JSON::PP->new->utf8->canonical->allow_nonref;
my $spec;
eval {
my $modules;
if($opt{list_extensions} || @extensions){
  my $helper="$RealBin/LLSynthExtensions.pm";
  require $helper;
  $modules=LLSynthExtensions::discover("$RealBin/../extensions",@extension_paths);
  if($opt{list_extensions}){print "$_\n" for sort keys %$modules;exit 0}
  exists $modules->{$_} or die "unknown extension '$_' (see --list-extensions)\n" for @extensions;
  push @generator_modules,$helper,values %$modules;
}
$opt{T}=lc $opt{T};
$opt{T}= 'python3' if $opt{T} eq 'python';
($opt{T} =~ /^(?:python3|c99|c11|cpp17|cpp20)$/) or die "E001 unsupported tool target '$opt{T}'\n";
($opt{mode}=~/^(?:interpret|compile|both|analyze|rewrite)$/) or die "E002 invalid mode\n";
($opt{split}=~/^(?:none|explicit|facets|all)$/) or die "E003 invalid split policy\n";
($opt{parser}=~/^(?:|lalr1|lr1|ll1|glr)$/) or die "E004 invalid parser\n";


($opt{driver_mode} =~ /^(?:cli|library|both)$/) or die "E007 invalid driver mode\n";


defined($opt{max_errors}) && $opt{max_errors} != 1 && die "E010 this release reports one error per invocation; use --max-errors 1\n";
($opt{diagnostics}=~/^(text|json|sarif)$/) or die "E012 invalid diagnostics format\n";
$opt{table_format} && $opt{table_format} ne 'source' && die "E011 custom table formats are not implemented\n";

  $spec=parse_spec($input);$spec->{trace_parser}=$opt{trace_parser}?1:0;
  if(@extensions){
    my %dependencies=map {$_->{path}=>$_} @{$spec->{dependencies}};
    for my $name(@extensions){
      my $path=canonical_path($modules->{$name});
      $dependencies{$path}={path=>$path,sha256=>sha256_hex(slurp($path))};
    }
    $spec->{dependencies}=[map {$dependencies{$_}} sort keys %dependencies];
  }
  validate_declarations($spec);
  my $p=$opt{parser}||$spec->{options}{parser}||'lalr1';
  $p =~ /^(?:lalr1|lr1)$/ or fail("unsupported parser '$p'", $input,0);
  # LR1 is used internally for all supported LR-family algorithms; LALR merges compatible cores.
  $spec->{parser_algorithm}=$p;
  compile_grammar($spec,$p);
  compile_lexer($spec);
  compile_actions($spec);
  compile_passes($spec);
  @{$spec->{analyses}} and fail('declarative .analysis blocks are not yet executable',$input,1);
  validate($spec);
  validate_actions($spec,$opt{mode});
  validate_rewrites($spec);
  validate_types($spec);
  validate_program_backend($spec);
if(defined $opt{dump_grammar}){
 my $val=join('',map {$_->{id}.': '.$_->{lhs}.' -> '.join(' ',@{$_->{rhs}})."\n"} @{$spec->{productions}});
 if(length $opt{dump_grammar}){write_file($opt{dump_grammar},$val)} else {print $val}
}
if(defined $opt{dump_tables}){
 my $val=$json->pretty->encode({productions=>$spec->{productions},actions=>$spec->{actions},gotos=>$spec->{gotos}});
 if(length $opt{dump_tables}){write_file($opt{dump_tables},$val)} else {print $val}
}
if($opt{check}){ print STDERR "OK: $spec->{language} (".scalar(@{$spec->{productions}})." productions, ".scalar(@{$spec->{actions}})." parser states)\n";exit 0 }
my @outputs;
if(@extensions) { @outputs=LLSynthExtensions::generate($modules,\@extensions,$spec,\%opt) }
elsif($opt{T} eq 'python3') { @outputs=generate_python($spec,\%opt,$json) }
else { @outputs=generate_c($spec,\%opt,$json) }
if($opt{manifest}) {
  my $manifest={generator=>'llsynth.pl',version=>$VERSION,language=>$spec->{language},
    dependencies=>$spec->{dependencies},source=>$input, source_sha256=>sha256_hex(slurp($input)), options=>{map { $_=>$opt{$_} } grep {defined $opt{$_} && !ref($opt{$_})} sort keys %opt},
    outputs=>[map { {path=>$_,sha256=>sha256_hex(slurp($_))} } @outputs]};
  $manifest->{options}{extensions}=[@extensions] if @extensions;
  my $path=outpath($opt{manifest});write_file($path,$json->pretty->encode($manifest));push @outputs,$path;
}
if($opt{depfile}) {my $dst=outpath($opt{depfile});my $target=$outputs[0]||'llsynth';write_file($dst,make_escape($target).": ".join(" ",map {make_escape($_->{path})} @{$spec->{dependencies}})."\n");push @outputs,$dst}
print STDERR "generated ".join(', ',@outputs)."\n";
exit 0;
};
if($@){report_error($@);exit 1}

sub report_error {
 my($msg)=@_;chomp $msg;
 my($file,$line)=($input,1);
 if($msg =~ /^(.*?):(\d+): error: (.*)/s){($file,$line,$msg)=($1,0+$2,$3)}
 my $code=$msg =~ /^(E\d+)\b/?$1:'LLS001';
 my $d={severity=>'error',code=>$code,message=>$msg,location=>{file=>$file,line=>$line,column=>1}};
 if($opt{diagnostics} eq 'json'){print STDERR $json->encode($d)."\n"}
 elsif($opt{diagnostics} eq 'sarif'){
  print STDERR $json->encode({version=>'2.1.0',runs=>[{tool=>{driver=>{name=>'llsynth.pl',version=>$VERSION}},results=>[
   {ruleId=>$code,level=>'error',message=>{text=>$msg},locations=>[{physicalLocation=>{artifactLocation=>{uri=>$file},region=>{startLine=>$line,startColumn=>1}}}]}]}]})."\n";
 }else{print STDERR "$file:$line: error: $msg\n"}
}
sub canonical_path {
 my($path)=@_;
 return Cwd::abs_path($path) if -e $path;
 my $parent=Cwd::abs_path(dirname($path));
 return defined $parent?File::Spec->catfile($parent,basename($path)):File::Spec->rel2abs($path);
}
sub make_escape {
 my($s)=@_;$s=~s/\$/\$\$/g;$s=~s/([ #:\\])/\\$1/g;return $s;
}

sub usage {
 my($status)=@_;
 my $h=<<'TXT';
Usage: llsynth.pl [options] FILE.llsynth
  -T, --tool-target LANG     python3, c99, c11, cpp17, cpp20
  -o, --output FILE          generated implementation
  -h, --header FILE          generated C/C++ public header (NOT --help)
  --mode MODE                interpret, compile, both, analyze, rewrite
  --program-target LANG      c99, c11, cpp17, cpp20, python3, bytecode
  --parser ALG               lr1 or lalr1
  --runtime FILE             optional generated C runtime source
  --semant FILE              optional generated C semantics source
  --table FILE               optional C includable parser tables
  --rewriter FILE            optional rewrite implementation
  --split none|explicit|facets|all layout of generated C/C++ outputs
  --check                    validate and exit
  --list-extensions          list available Perl extensions (no input file)
  --extension NAME           generate extension artifacts (repeatable)
  --extension-path DIR       discover additional Perl extensions (repeatable)
  --dump-tables[=FILE]       show generated parser tables
  --dump-grammar[=FILE]      show expanded grammar
  --out-dir DIR              directory for emitted files
  --driver-mode MODE         cli, library, both
  --package NAME             generate a Python package
  --module-prefix NAME       C/C++ symbol prefix
  --trace-parser             trace generated parser actions
  --include-path DIR         additional textual include root
  --module-path DIR          additional specification module root (repeatable)
  --depfile FILE             Make/Ninja dependencies
  --manifest FILE            output SHA256 manifest
  --diagnostics text|json|sarif generator error format
  --help, --version          information
TXT
 print $h;exit $status;
}
sub fail {my($s,$f,$line)=@_; die "".($f||'<input>').':'.($line||1).": error: $s\n"}
sub slurp {my($f)=@_;open my $fh,'<:raw',$f or die "$f: $!\n";local $/;my $s=<$fh>;close $fh;return $s}
sub protected_inputs {
 return map {canonical_path($_)=>1}
  ($input,__FILE__,@generator_modules,map {$_->{path}} @{$spec->{dependencies}||[]});
}
sub write_file {
 my($f,$s)=@_;
 my %inputs=protected_inputs();
 !$inputs{canonical_path($f)} or die "output would overwrite an input: $f\n";
 -d dirname($f) or die "output directory does not exist: ".dirname($f)."\n";
 my($fh,$tmp)=tempfile('.llsynth-XXXXXXXX',DIR=>dirname($f),UNLINK=>0);
 binmode $fh;
 my $ok=eval {print $fh $s or die "$tmp: $!\n";close $fh or die "$tmp: $!\n";rename $tmp,$f or die "$tmp -> $f: $!\n";1};
 if(!$ok){my $error=$@;close $fh;unlink $tmp;die $error}
}

sub outpath {my($f)=@_;return $f if $f =~ m{^(?:/|\./|\.\./)};return "$opt{out_dir}/$f"}

sub cstr {my($s)=@_;$s =~ s/\\/\\\\/g;$s =~ s/"/\\"/g;$s =~ s/\n/\\n/g;$s =~ s/\r/\\r/g;$s =~ s/\t/\\t/g;return '"'.$s.'"'}
sub relative_include {
 my($file,$owner)=@_;
 return cstr(File::Spec->abs2rel(File::Spec->rel2abs($file),File::Spec->rel2abs(dirname($owner))));
}
sub write_outputs {
 my($files,$main)=@_;
 my %seen;
 my %inputs=protected_inputs();
 for my $facet(qw(manifest depfile)){next unless defined $opt{$facet};my $path=outpath($opt{$facet});
  -d dirname($path) or die "output directory does not exist: ".dirname($path)."\n";
  my $abs=canonical_path($path);$seen{$abs}++ and die "output path collision: $path\n";
  $inputs{$abs} and die "output would overwrite an input: $path\n";
 }
 for my $path(sort keys %$files){
  -d dirname($path) or die "output directory does not exist: ".dirname($path)."\n";
  my $abs=canonical_path($path);
  $seen{$abs}++ and die "output path collision: $path\n";
  $inputs{$abs} and die "output would overwrite an input: $path\n";
 }
 for my $path(sort keys %$files){write_file($path,$files->{$path})}
 return ($main,grep {$_ ne $main} sort keys %$files);
}

# The compiler and emitter are sibling Perl modules. Runtime templates stay
# embedded in the driver's DATA section; no template files are needed.
our %lls_templates;
sub template {
 my($target)=@_;
 unless(%lls_templates){
  local $/;
  my $data=<DATA>;
  $data =~ s/^\@\@LLS_TEMPLATE_PYTHON\@\@\n// or die "missing embedded Python template\n";
  my ($python,$c)=split /^\@\@LLS_TEMPLATE_C\@\@\n/m,$data,2;
  defined $c or die "missing embedded C template\n";
  %lls_templates=(python=>$python,c=>$c);
 }
 exists $lls_templates{$target} or die "unknown embedded template '$target'\n";
 return $lls_templates{$target};
}

__DATA__
@@LLS_TEMPLATE_PYTHON@@
#!/usr/bin/env python3
"""Generated by llsynth.pl. No third-party packages required."""
from __future__ import annotations
import argparse
import base64
import json
import math
import struct
import re
import sys
import copy
import threading
import time
from collections.abc import MutableMapping
from dataclasses import dataclass, field
from typing import Any

SPEC = json.loads(base64.b64decode("@@SPEC_BASE64@@"))
MODE = "@@MODE@@"
DEFAULT_TARGET = "@@TARGET@@"

class LanguageError(Exception):
    def __init__(self, code, message, line=0, column=0, frames=()):
        super().__init__(message)
        self.code, self.line, self.column, self.frames = code, line, column, tuple(frames)
    def __str__(self):
        loc = f"{self.line}:{self.column}: " if self.line else ""
        return f"{loc}{self.code}: {super().__str__()}" + (" [rules: " + " -> ".join(self.frames) + "]" if self.frames else "")

@dataclass
class Token:
    kind: str
    lexeme: str
    value: Any
    line: int
    column: int

@dataclass
class Node:
    symbol: str
    children: list[Node] = field(default_factory=list)
    prod: int = -1
    token: Token | None = None
    value: Any = None
    parent: Node | None = field(default=None, repr=False, compare=False)
    def __post_init__(self):
        for child in self.children: child.parent = self
        if self.token is None:
            self.token = next((ch.token for ch in self.children if ch.token is not None), None)

@dataclass
class Operation:
    name: str
    args: tuple[Any, ...] = ()

@dataclass(frozen=True)
class DataValue:
    owner: str
    tag: str
    fields: tuple[Any, ...]

@dataclass
class FunctionRef:
    name: str

@dataclass
class Cell:
    value: Any

class Environment(MutableMapping):
    """A lexical scope owns a name map; closures retain the referenced cells."""
    def __init__(self, values=()):
        self.cells = {k: Cell(v) for k, v in dict(values).items()}
    def __getitem__(self, key): return self.cells[key].value
    def __setitem__(self, key, value):
        if key in self.cells: self.cells[key].value = value
        else: self.define(key, value)
    def __delitem__(self, key): del self.cells[key]
    def __iter__(self): return iter(self.cells)
    def __len__(self): return len(self.cells)
    def define(self, key, value): self.cells[key] = Cell(value)
    def capture(self, names=None):
        result = Environment()
        result.cells = {k: v for k, v in self.cells.items() if names is None or k in names}
        return result

@dataclass
class Closure:
    definition: dict
    env: Environment
    caps: dict
    owner: Any
    generation: Any
    def __deepcopy__(self, memo):
        if id(self) in memo: return memo[id(self)]
        result = Closure(self.definition, Environment(), {}, memo.get(id(self.owner), self.owner), self.generation)
        memo[id(self)] = result
        result.env = copy.deepcopy(self.env, memo)
        result.caps = copy.deepcopy(self.caps, memo)
        return result

class ExecutionBudget:
    def __init__(self):
        self.fuel = 1000000
        self.tasks = []
        self.lock = threading.Lock()
        self.sleep_seconds = 0

class Task:
    """Single-shot lazy computation. No mutable DSL value crosses a thread boundary."""
    def __init__(self, owner, callee, args):
        self.owner, self.generation = owner, owner.budget
        self.thread = None
        self.observed = False
        self.value, self.error = None, None
        self.lock = threading.Lock()
        self.context = object.__new__(Context)
        child = self.context
        child.budget, child.depth, child.node = owner.budget, 0, owner.node
        memo = {id(owner): child}
        try:
            child.state, child.tables, self.callee, self.args = copy.deepcopy(
                (owner.state, owner.tables, callee, args), memo)
        except LanguageError: raise
        except Exception as exc: raise LanguageError('TASK', f'value cannot be copied into task: {exc}') from None
        with owner.budget.lock:
            if len(owner.budget.tasks) >= 64:
                raise LanguageError('RESOURCE', 'task creation limit (64 per execution) exceeded')
            owner.budget.tasks.append(self)
    def __deepcopy__(self, memo):
        raise LanguageError('TASK', 'task handles cannot cross task boundaries')
    def start(self):
        with self.lock:
            if self.thread is None:
                self.thread = threading.Thread(target=self.run, name='llsynth-task')
                try: self.thread.start()
                except RuntimeError as exc:
                    self.thread = None
                    raise LanguageError('RESOURCE', f'cannot start task: {exc}') from None
        return self
    def run(self):
        try: self.value = self.context.invoke(self.callee, self.args, run_async=True)
        except Exception as exc:
            self.error = exc if isinstance(exc, LanguageError) else LanguageError('SEM', str(exc))
    def result(self, receiver):
        if receiver.budget is not self.generation:
            raise LanguageError('TASK', 'task belongs to another execution')
        self.start()
        self.thread.join()
        self.observed = True
        if self.error: raise self.error
        try: return copy.deepcopy(self.value, {id(self.context): receiver})
        except LanguageError: raise
        except Exception as exc: raise LanguageError('TASK', f'task result cannot be copied: {exc}') from None

def closure_names(node, bound=()):
    """Free lexical names, respecting parameter/local shadowing and declaration order."""
    bound = set(bound)
    if isinstance(node, list):
        names = set()
        for item in node:
            names.update(closure_names(item, bound))
            if item.get('k') == 'let': bound.add(item['name'])
            if item.get('k') == 'grab': bound.add(item['var'])
        return names
    if not isinstance(node, dict): return set()
    kind = node.get('k')
    if kind == 'closure':
        return closure_names(node['body'], {p['name'] for p in node['params']}) - bound
    if kind == 'for':
        return closure_names(node['items'], bound) | closure_names(node['body'], bound | {node['var']})
    names = {node['v']} - bound if kind == 'name' else set()
    for value in node.values():
        if isinstance(value, (list, dict)): names.update(closure_names(value, bound))
    return names

def lex(source: str) -> list[Token]:
    """Walk generated deterministic byte-transition tables (maximal munch)."""
    try: data = source.encode('utf-8')
    except UnicodeError as exc: raise LanguageError('LEX', f'invalid UTF-8: {exc}') from None
    dfa = SPEC['lexdfa']
    rows, rules = dfa['states'], dfa['rules']
    result = []
    pos, line, col = 0, 1, 1
    while pos < len(data):
        state, length, best_rule, best_length = 0, 0, -1, 0
        while True:
            for rule_id in rows[state]['accept']:
                rule = rules[rule_id]
                if rule['bol'] and not (pos == 0 or data[pos-1] == 10):
                    continue
                if rule['eol'] and not (pos+length == len(data) or data[pos+length] == 10):
                    continue
                if length > best_length:
                    best_rule, best_length = rule_id, length
                elif length == best_length and best_rule >= 0 and rule_id < best_rule:
                    best_rule = rule_id
            if pos+length >= len(data):
                break
            char = data[pos+length]
            nxt = -1
            for lo, hi, dest in rows[state]['trans']:
                if lo <= char <= hi:
                    nxt = dest
                    break
            if nxt < 0:
                break
            state, length = nxt, length + 1
        if best_rule < 0:
            shown = data[pos:pos+1].decode('utf-8', 'backslashreplace')
            raise LanguageError('LEX', f"unexpected byte {shown!r}", line, col)
        rule = rules[best_rule]
        raw = data[pos:pos+best_length]
        try:
            part = raw.decode('utf-8')
        except UnicodeDecodeError:
            raise LanguageError('LEX', 'lexer split a UTF-8 sequence', line, col)
        if not rule['skip']:
            value = part
            if rule['type'] == 'Bool':
                if part not in ('true', 'false'): raise LanguageError('LEX', 'invalid Bool literal', line, col)
                value = part == 'true'
            if rule['type'] in ('Float64', 'Float32', 'Int', 'Int64'):
                try:
                    value = float(part) if rule['type'].startswith('Float') else checked_int(int(part))
                    if isinstance(value, float) and not math.isfinite(value):
                        raise ValueError('non-finite numeric literal')
                except ValueError:
                    raise LanguageError('LEX', f"invalid numeric literal {part!r}", line, col)
            result.append(Token(rule['name'], part, value, line, col))
        for ch in part:
            if ch == '\n': line, col = line+1, 1
            else: col += 1
        pos += best_length
    result.append(Token('EOF', '', None, line, col))
    result.append(Token('$', '', None, line, col))
    return result

def parse(source: str) -> Node:
    tokens = lex(source)
    states = [0]
    nodes = []
    index = 0
    while True:
        if len(states) > 200000:
            raise LanguageError('PARSE', 'parser stack exceeded safety limit')
        tok = tokens[index]
        act = SPEC['actions'][states[-1]].get(tok.kind)
        if act is None:
            choices = ', '.join(sorted(SPEC['actions'][states[-1]]))
            raise LanguageError('PARSE', f"unexpected {tok.kind} ({tok.lexeme!r}); expected {choices}", tok.line, tok.column)
        typ, arg = act
        if SPEC.get('trace_parser'): print(f'parser: state={states[-1]} token={tok.kind} action={typ}:{arg}', file=sys.stderr)
        if typ == 's':
            nodes.append(Node(tok.kind, token=tok, value=tok.value))
            states.append(arg)
            index += 1
        elif typ == 'r':
            prod = SPEC['productions'][arg]
            n = len(prod['rhs'])
            children = nodes[-n:] if n else []
            if n:
                del nodes[-n:]
                del states[-n:]
            node = Node(prod['lhs'], children, prod['id'])
            nxt = SPEC['gotos'][states[-1]].get(prod['lhs'])
            if nxt is None:
                raise LanguageError('PARSE', 'invalid generated goto table')
            nodes.append(node)
            states.append(nxt)
        elif typ == 'a':
            return nodes[-1]
        else:
            raise LanguageError('PARSE', 'invalid generated parse action')

@@INTEROP_PYTHON@@

class Context:
    @property
    def fuel(self): return self.budget.fuel
    @fuel.setter
    def fuel(self, value): self.budget.fuel = value
    def __init__(self):
        self.state = {}
        self.tables = {}
        self.node = None
        self.reset()
    def reset(self):
        if hasattr(self, 'budget'): self.finish_tasks()
        self.budget = ExecutionBudget()
        self.node = None
        self.depth = 0
        self.state = {}
        for item in SPEC['states']:
            self.state[item['name']] = self.eval(item['initial_expr'], {}, {})
        self.tables = {item['name']: {} for item in SPEC['tables']}
        self.node = None
        try: self.exec_block(SPEC['init_code'], {}, {})
        except BaseException:
            try: self.finish_tasks()
            except Exception: pass
            raise
        else: self.finish_tasks()
    def raise_error(self, name, args):
        key = name.lstrip('!')
        definition = next((x for x in SPEC['exceptions'] if x['name'] == key), None)
        if definition:
            message = definition['message']
            fields = [v.split(':', 1)[0].strip() for v in definition['params'].split(',') if v.strip()]
            for k, v in zip(fields, args):
                message = message.replace('{' + k + '}', format_value(v))
        else:
            message = f'{key}: {args}'
        tok = self.first_token(self.node) if self.node else None
        frames, node = [], self.node
        while node:
            if node.prod >= 0 and node.symbol not in SPEC['internal']: frames.append(node.symbol)
            node = node.parent
        raise LanguageError('!' + key, message, tok.line if tok else 0, tok.column if tok else 0, reversed(frames))
    @staticmethod
    def first_token(node):
        if node is None:
            return None
        if node.token:
            return node.token
        for ch in node.children:
            tok = Context.first_token(ch)
            if tok:
                return tok
        return None
    def capture(self, name, caps):
        if name not in caps:
            raise LanguageError('SEM', f'capture ${name} is not bound in this production')
        return caps[name]
    def eval(self, e, env, caps):
        k = e['k']
        if k == 'num': return int(e.get('literal', e['v'])) if e.get('type') == 'Int' else float(e['v'])
        if k == 'str': return e['v']
        if k == 'list': return [self.eval(t, env, caps) for t in e['args']]
        if k == 'closure':
            scope = env if isinstance(env, Environment) else Environment(env)
            return Closure(e, scope.capture(closure_names(e)), dict(caps), self, self.budget)
        if k == 'await':
            return self.task(self.eval(e['a'], env, caps)).result(self)
        if k == 'name':
            name = e['v']
            if name.startswith('@'): return self.state[name[1:]]
            if name.startswith('#'): return self.tables[name[1:]]
            if name.startswith('$'): return self.capture(name[1:], caps)
            if name.startswith('&'): return FunctionRef(name[1:])
            if name in SPEC['type_ops']: return FunctionRef(name)
            if name.startswith('t\''): return env[name]
            if name == 'true': return True
            if name == 'false': return False
            if name in ('None', 'nil'): return None
            if name in SPEC['op_by_name']: return Operation(name)
            if name in env: return env[name]
            if any(f['name'] == name for f in SPEC['functions']): return FunctionRef(name)
            return name
        if k == 'un':
            a = self.eval(e['a'], env, caps)
            return checked_int(-a) if e['v'] == '-' and type(a) is int else -a if e['v'] == '-' else not a
        if k == 'index':
            return self.eval(e['a'], env, caps)[self.eval(e['b'], env, caps)]
        if k == 'bin':
            op = e['v']
            a = self.eval(e['a'], env, caps)
            if op == '&&': return bool(a) and bool(self.eval(e['b'], env, caps))
            if op == '||': return bool(a) or bool(self.eval(e['b'], env, caps))
            b = self.eval(e['b'], env, caps)
            if e.get('type') == 'Int' or (e.get('type') == 'Any' and type(a) is int and type(b) is int):
                if op == '/':
                    if b == 0: raise LanguageError('SEM', 'division by zero')
                    return checked_int((abs(a) // abs(b)) * (-1 if (a < 0) != (b < 0) else 1))
                return checked_int({'+': lambda: a+b, '-': lambda: a-b, '*': lambda: a*b}[op]())
            return {'+': lambda: a+b, '-': lambda: a-b, '*': lambda: a*b,
                    '/': lambda: a/b, '==': lambda: a==b, '!=': lambda: a!=b,
                    '<': lambda: a<b, '>': lambda: a>b, '<=': lambda: a<=b,
                    '>=': lambda: a>=b, 'in': lambda: a in b}[op]()
        if k == 'call':
            callee = self.eval(e['a'], env, caps)
            args = [self.eval(item, env, caps) for item in e['args']]
            if isinstance(callee, Operation): return Operation(callee.name, tuple(args))
            if isinstance(callee, FunctionRef):
                return self.invoke(callee, args)
            if isinstance(callee, Closure): return self.invoke(callee, args)
            if callee == 'spawn': return self.task(args[0]).start()
            if callee == 'join': return self.task(args[0]).result(self)
            if callee == 'sleep':
                seconds = args[0]
                if type(seconds) not in (int, float) or not math.isfinite(seconds) or not 0 <= seconds <= 1:
                    raise LanguageError('TASK', 'sleep requires finite seconds in [0, 1]')
                with self.budget.lock:
                    self.budget.sleep_seconds += seconds
                    if self.budget.sleep_seconds > 10:
                        raise LanguageError('RESOURCE', 'aggregate sleep budget (10 seconds) exhausted')
                time.sleep(seconds)
                return None
            if callee == 'is_finite': return math.isfinite(args[0])
            if callee == 'abs': return checked_int(abs(args[0])) if type(args[0]) is int else abs(args[0])
            if callee == 'min': return min(args)
            if callee == 'max': return max(args)
            if callee == 'reverse': return list(reversed(args[0]))
            if callee == 'len': return len(args[0])
            if callee == 'pop':
                if not args[0]: self.raise_error('!stack_underflow', ['pop'])
                return args[0].pop()
            if callee == 'top':
                if not args[0]: self.raise_error('!stack_underflow', ['top'])
                return args[0][-1]
            if callee in ('push', 'emit'):
                args[0].append(args[1]); return None
            if callee == 'print':
                print(format_value(args[0])); return None
            if callee == 'final_stack_height': return verify_ops(args[0])
            raise LanguageError('SEM', f'undefined callable {callee!r}')
        raise LanguageError('SEM', f'invalid expression node {k}')
    def task(self, value):
        if not isinstance(value, Task): raise LanguageError('TASK', 'expected a Task')
        if value.generation is not self.budget: raise LanguageError('TASK', 'task belongs to another execution')
        return value
    def invoke(self, callee, args, run_async=False):
        if isinstance(callee, FunctionRef) and callee.name in SPEC['type_ops']:
            op = SPEC['type_ops'][callee.name]
            if len(args) != len(op['params']):
                raise LanguageError('SEM', f"{callee.name} expects {len(op['params'])} arguments")
            if op['action'] == 'unit': return None
            if op['action'] == 'make': return DataValue(op['owner'], op['tag'], tuple(args))
            data = args[0]
            if not isinstance(data, DataValue) or data.owner != op['owner']:
                raise LanguageError('SEM', f"expected a value of {op['owner']}")
            if op['action'] == 'is': return data.tag == op['tag']
            if data.tag != op['tag']: raise LanguageError('SEM', f"expected {op['tag']}, got {data.tag}")
            return data.fields[op['index']]
        if isinstance(callee, Closure):
            if callee.owner is not self or callee.generation is not self.budget:
                raise LanguageError('SEM', 'closure belongs to another context or execution')
            definition = callee.definition
            keys = [p['name'] for p in definition['params']]
            env, caps, code = callee.env.capture(), callee.caps, definition['body']
        else:
            definition = next((f for f in SPEC['functions'] if f['name'] == callee.name), None)
            if not definition: raise LanguageError('SEM', f'undefined function {callee.name}')
            keys = [p['name'] for p in definition['parameters']]
            env, caps, code = Environment(), {}, definition.get('code', [])
        if len(keys) != len(args):
            raise LanguageError('SEM', f'function expects {len(keys)} arguments, got {len(args)}')
        if definition.get('foreign'): return interop_call(definition, args)
        if definition.get('async') and not run_async: return Task(self, callee, args)
        for key, value in zip(keys, args): env.define(key, value)
        if self.depth >= 128: raise LanguageError('RESOURCE', 'function recursion limit exceeded')
        self.depth += 1
        try: return self.exec_block(code, env, caps)[1]
        finally: self.depth -= 1
    def finish_tasks(self):
        # Tasks may create children while being joined; the registry only grows.
        first_error, index = None, 0
        while index < len(self.budget.tasks):
            task = self.budget.tasks[index]
            index += 1
            if task.thread is not None:
                task.thread.join()
                if task.error and not task.observed and first_error is None: first_error = task.error
                task.observed = True
        if first_error is not None: raise first_error
    def tick(self):
        with self.budget.lock:
            self.budget.fuel -= 1
            if self.budget.fuel < 0: raise LanguageError('RESOURCE', 'action instruction budget exhausted')
    def exec_block(self, code, env, caps):
        if not isinstance(env, Environment): env = Environment(env)
        for stmt in code:
            self.tick()
            k = stmt['k']
            if k == 'on':
                if stmt['name'] in caps:
                    done, value = self.exec_block(stmt['body'], env, caps)
                    if done: return True, value
            elif k == 'require':
                if not self.eval(stmt['cond'], env, caps):
                    self.raise_error(stmt['name'], [self.eval(a, env, caps) for a in stmt['args']])
            elif k == 'throw':
                self.raise_error(stmt['name'], [self.eval(a, env, caps) for a in stmt['args']])
            elif k == 'let': env.define(stmt['name'], self.eval(stmt['expr'], env, caps))
            elif k == 'grab':
                key = self.eval(stmt['key'], env, caps)
                table = self.tables[stmt['table'][1:]]
                if key not in table: raise LanguageError('SEM', f'key {key!r} not in {stmt["table"]}')
                env[stmt['var']] = table[key]
            elif k == 'assign':
                lhs = stmt['lhs']; value = self.eval(stmt['rhs'], env, caps)
                if lhs['k'] == 'index':
                    self.eval(lhs['a'], env, caps)[self.eval(lhs['b'], env, caps)] = value
                elif lhs['k'] == 'name' and lhs['v'].startswith('@'):
                    self.state[lhs['v'][1:]] = value
                else: env[lhs['v']] = value
            elif k == 'exec': self.eval(stmt['expr'], env, caps)
            elif k == 'return': return True, self.eval(stmt['expr'], env, caps)
            elif k == 'while':
                while self.eval(stmt['cond'], env, caps):
                    self.tick()
                    done, val = self.exec_block(stmt['body'], env, caps)
                    if done: return True, val
            elif k == 'for':
                for item in list(self.eval(stmt['items'], env, caps)):
                    self.tick()
                    env[stmt['var']] = item
                    done, val = self.exec_block(stmt['body'], env, caps)
                    if done: return True, val
            elif k == 'if':
                done, val = self.exec_block(stmt['yes'] if self.eval(stmt['cond'], env, caps) else stmt['no'], env, caps)
                if done: return True, val
            else: raise LanguageError('SEM', f'unknown semantic statement {k}')
        return False, None
    def walk(self, root, facet):
        try: self._walk(root, facet)
        except BaseException:
            try: self.finish_tasks()
            except Exception: pass
            raise
        else: self.finish_tasks()
    def _walk(self, root, facet):
        pending = [(root, False)]
        while pending:
            node, ready = pending.pop()
            if ready:
                try: self.walk_node(node, facet)
                except (LanguageError, ValueError, IndexError, KeyError, ZeroDivisionError) as original:
                    exc = original if isinstance(original, LanguageError) else LanguageError('SEM', str(original))
                    token = node.token
                    if not exc.line and token: exc.line, exc.column = token.line, token.column
                    if not exc.frames:
                        frames, frame = [], node
                        while frame:
                            if frame.prod >= 0 and frame.symbol not in SPEC['internal']: frames.append(frame.symbol)
                            frame = frame.parent
                        exc.frames = tuple(reversed(frames))
                    raise exc from None
            else:
                pending.append((node, True))
                pending.extend((ch, False) for ch in reversed(node.children))
    def walk_node(self, node, facet):
        old = self.node
        self.node = node
        if node.prod >= 0:
            prod = SPEC['productions'][node.prod]
            internal = SPEC.get('internal', {}).get(prod['lhs'])
            if internal is not None:
                values = [ch.value for ch in node.children]
                if internal in ('*', '+', 'sep'):
                    if not values: node.value = []
                    elif len(values) == 1: node.value = [values[0]]
                    else:
                        node.value = values[0]
                        node.value.append(values[-1])
                elif internal == '?': node.value = values[0] if values else None
                else: node.value = values[0] if len(values) == 1 else values
            definition = next((r for r in SPEC['rules'] if r['name'] == prod['lhs']), None)
            if definition:
                caps = {}
                for label, ch in zip(prod['labels'], node.children):
                    if label: caps[label] = ch.value
                actions = definition.get('code', {}).get(facet, [])
                _, val = self.exec_block(actions, {}, caps)
                node.value = val
        self.node = old
    def analyze(self, source: str):
        self.reset()
        root = parse(source)
        self.walk(root, 'analyze')
        return root
    def interpret(self, source: str):
        if MODE not in ('both', 'interpret'):
            raise LanguageError('MODE', 'interpreter is not generated')
        self.reset()
        root = parse(source)
        self.walk(root, 'semantics')
        return root.value
    def rewrite(self, ops):
        passes = SPEC.get('rewrites_compiled', [])
        if not passes or not SPEC.get('rewrite_enabled', True):
            return ops
        ops = list(ops)
        seen = set()
        fuel = SPEC.get('rewrite_fuel', 10000)
        def signature():
            return tuple((op.name, tuple(struct.pack('<d', a) if type(a) is float else (type(a).__name__, repr(a)) for a in op.args)) for op in ops)
        for iteration in range(SPEC.get('rewrite_fuel', 10000) + 1):
            sig = signature()
            if sig in seen:
                raise LanguageError('REWRITE', 'rewrite cycle detected')
            seen.add(sig)
            changed = False
            for rule in passes:
                if rule['phase'] != 'optimize':
                    continue
                count = len(rule['match'])
                i = 0
                while i <= len(ops)-count:
                    env = {'fp_mode': SPEC['options'].get('fp_mode', 'strict')}
                    ok = True
                    for actual, pattern in zip(ops[i:i+count], rule['match']):
                        if actual.name != pattern['op'] or len(actual.args) != len(pattern['args']):
                            ok = False
                            break
                        for arg, pat in zip(actual.args, pattern['args']):
                            if pat.isidentifier():
                                if pat in env and env[pat] != arg:
                                    ok = False
                                env[pat] = arg
                            elif pat != format_value(arg):
                                ok = False
                    if ok and self.eval(rule['when'], env, {}):
                        replaced = [Operation(p['op'], tuple(self.eval(a, env, {}) for a in p['args']))
                                    for p in rule['replace']]
                        def effect(seq):
                            return sum(SPEC['op_by_name'][op.name]['push']-
                                       SPEC['op_by_name'][op.name]['pop'] for op in seq)
                        if effect(replaced) != effect(ops[i:i+count]):
                            raise LanguageError('REWRITE', 'rewrite changes IR stack height')
                        if [(o.name, tuple(struct.pack('<d', a) if type(a) is float else (type(a).__name__, repr(a)) for a in o.args)) for o in ops[i:i+count]] != [(o.name, tuple(struct.pack('<d', a) if type(a) is float else (type(a).__name__, repr(a)) for a in o.args)) for o in replaced]:
                            fuel -= 1
                            if fuel < 0: raise LanguageError('REWRITE', 'rewrite fuel exhausted')
                            ops[i:i+count] = replaced
                            changed = True
                        i += max(1, len(replaced))
                    else:
                        i += 1
            if not changed or not SPEC.get('rewrite_repeat', True):
                return ops
        raise LanguageError('REWRITE', 'rewrite iteration budget exhausted')
    def compile_ir(self, source: str):
        if MODE not in ('both', 'compile', 'rewrite'):
            raise LanguageError('MODE', 'compiler is not generated')
        self.reset()
        root = parse(source)
        self.walk(root, 'lower')
        code = root.value if isinstance(root.value, list) else self.state.get('code', [])
        verify_ops(code)
        code = self.rewrite(code)
        verify_ops(code)
        if 'code' in self.state:
            self.state['code'] = code
        return code
    def compile(self, source: str, target: str = DEFAULT_TARGET):
        ops = self.compile_ir(source)
        return compile_program(ops, target)

def checked_int(value):
    if not -(1 << 63) <= value < (1 << 63):
        raise LanguageError('INTEGER_OVERFLOW', 'signed 64-bit integer out of range')
    return value

def verify_ops(ops):
    stack = []
    for op in ops:
        if not isinstance(op, Operation) or op.name not in SPEC['op_by_name']:
            raise LanguageError('IR', 'unknown IR operation')
        info = SPEC['op_by_name'][op.name]
        params = [p.split(':', 1)[1].strip() for p in info['params'].split(',') if p.strip()]
        if len(params) != len(op.args):
            raise LanguageError('IR', f'wrong operand count for {op.name}')
        for typ, value in zip(params, op.args):
            valid = (typ == 'Float64' and type(value) in (int, float) or
                     typ == 'Int' and type(value) is int and -(1 << 63) <= value < (1 << 63) or
                     typ == 'String' and isinstance(value, str) or
                     typ == 'Bool' and type(value) is bool)
            if not valid:
                raise LanguageError('IR', f'invalid {typ} operand for {op.name}')
        inputs = info['inputs']
        if len(stack) < len(inputs):
            raise LanguageError('!stack_underflow', f'not enough operands for {op.name}')
        if inputs:
            if stack[-len(inputs):] != inputs:
                raise LanguageError('IR', f'stack type mismatch at {op.name}')
            del stack[-len(inputs):]
        stack.extend(info['outputs'])
    return len(stack)

def interpret(source):
    return Context().interpret(source)

def compile(source, target=DEFAULT_TARGET):
    return Context().compile(source, target)

def format_value(value):
    if type(value) is bool: return 'true' if value else 'false'
    if isinstance(value, float): return format(value, '.15g')
    return str(value)

BYTECODE_OPS = ('PUSH_F64', 'ADD', 'SUB', 'MUL', 'DIV', 'NEG', 'DUP', 'SWAP', 'DROP', 'PRINT')
BYTECODE_EFFECTS = ((0,1), (2,1), (2,1), (2,1), (2,1), (1,1), (1,2), (2,2), (1,0), (1,1))

def encode_bytecode(ops):
    if len(ops) > 1000000: raise LanguageError('BYTECODE', 'instruction limit exceeded')
    data = bytearray(b'LLSBC\x01' + struct.pack('<I', len(ops)))
    for op in ops:
        name = op.name.rsplit('.', 1)[-1]
        if name not in BYTECODE_OPS: raise LanguageError('BYTECODE', f'no VM implementation for {op.name}')
        code = BYTECODE_OPS.index(name)
        declaration = SPEC['op_by_name'][op.name]
        if (declaration['pop'], declaration['push']) != BYTECODE_EFFECTS[code] or any(t != 'Float64' for t in declaration['inputs'] + declaration['outputs']):
            raise LanguageError('BYTECODE', f'incompatible VM signature for {op.name}')
        data.append(code)
        if code == 0: data.extend(struct.pack('<d', op.args[0]))
    return bytes(data)

def execute_bytecode(data):
    if len(data) < 10 or data[:6] != b'LLSBC\x01': raise LanguageError('BYTECODE', 'invalid bytecode header')
    count, = struct.unpack_from('<I', data, 6)
    if count > 1000000: raise LanguageError('BYTECODE', 'instruction limit exceeded')
    pos, height = 10, 0
    instructions = []
    for _ in range(count):
        if pos >= len(data): raise LanguageError('BYTECODE', 'truncated instruction')
        code, pos = data[pos], pos+1
        if code >= len(BYTECODE_OPS): raise LanguageError('BYTECODE', 'unknown instruction')
        value = None
        if code == 0:
            if pos+8 > len(data): raise LanguageError('BYTECODE', 'truncated Float64 operand')
            value, = struct.unpack_from('<d', data, pos)
            pos += 8
        pop, push = BYTECODE_EFFECTS[code]
        if height < pop: raise LanguageError('BYTECODE', 'stack underflow in bytecode')
        height += push-pop
        instructions.append((code, value))
    if pos != len(data) or height != 1: raise LanguageError('BYTECODE', 'invalid bytecode length or final stack height')
    stack = []
    for code, value in instructions:
        if code == 0: stack.append(value)
        elif code == 5: stack[-1] = -stack[-1]
        elif code == 6: stack.append(stack[-1])
        elif code == 7: stack[-1], stack[-2] = stack[-2], stack[-1]
        elif code == 8: stack.pop()
        elif code == 9: print(format_value(stack[-1]))
        else:
            b, a = stack.pop(), stack.pop()
            if code == 4 and b == 0: raise LanguageError('!division_by_zero', 'Division by zero')
            stack.append({1: lambda: a+b, 2: lambda: a-b, 3: lambda: a*b, 4: lambda: a/b}[code]())
    return stack[0]

def number_literal(value, target):
    value = float(value)
    if not math.isfinite(value):
        if target == 'python3': return f'float({str(value)!r})'
        return 'NAN' if math.isnan(value) else '(-INFINITY)' if value < 0 else 'INFINITY'
    text = format(value, '.17g')
    return text if any(c in text for c in '.eE') else text + '.0'

def backend_value(value, target):
    if isinstance(value, str):
        if target == 'python3': return repr(value)
        return '"' + ''.join('\\'+c if c in '"\\' else c if 32 <= ord(c) < 127 else f'\\{ord(c):03o}' for c in value.encode('utf-8').decode('latin1')) + '"'
    if type(value) is bool: return ('True' if value else 'False') if target == 'python3' else ('1' if value else '0')
    if isinstance(value, float): return number_literal(value, target)
    return str(value)

def compile_program(ops, target):
    verify_ops(ops)
    for backend in SPEC.get('backends_compiled', []):
        if backend['target'] == target:
            pieces = [backend['prologue']]
            for op in ops:
                if not op.name.startswith(backend['ir'] + '.'): raise LanguageError('BACKEND', 'operation belongs to another IR')
                opname = op.name.rsplit('.', 1)[-1]
                mapping = backend['emits'].get(opname)
                if mapping is None:
                    raise LanguageError('BACKEND', f'no {target} emission mapping for {op.name}')
                if len(mapping['params']) != len(op.args):
                    raise LanguageError('BACKEND', f'operand mismatch for {op.name}')
                params = dict(zip(mapping['params'], op.args))
                pieces.append(re.sub(r'\$\{([A-Za-z_]\w*)\}',
                                     lambda m: backend_value(params[m.group(1)], target),
                                     mapping['template']))
            pieces.append(backend['epilogue'])
            return ''.join(pieces)
    if target == 'bytecode': return encode_bytecode(ops)
    if target not in ('c99', 'c11', 'cpp17', 'cpp20', 'python3'):
        raise LanguageError('BACKEND', f'unsupported program target {target}')
    # Built-in portable StackIR emitter. Other opcodes require a declarative backend.
    builtins = {'PUSH_F64', 'ADD', 'SUB', 'MUL', 'DIV', 'NEG', 'DUP', 'SWAP', 'DROP', 'PRINT'}
    for op in ops:
        name = op.name.split('.')[-1]
        if name in BYTECODE_OPS:
            decl = SPEC['op_by_name'][op.name]
            code = BYTECODE_OPS.index(name)
            if (decl['pop'], decl['push']) != BYTECODE_EFFECTS[code] or len(op.args) != (1 if code == 0 else 0) or any(t != 'Float64' for t in decl['inputs'] + decl['outputs']):
                raise LanguageError('BACKEND', f'incompatible built-in signature for {op.name}')
        if name not in builtins:
            raise LanguageError('BACKEND', f'no portable stack backend for {op.name}')
    if target == 'python3':
        code = ['#!/usr/bin/env python3', 'import sys', 's=[]']
        for op in ops:
            k = op.name.split('.')[-1]
            if k == 'PUSH_F64': code.append(f's.append({number_literal(op.args[0], target)})')
            elif k == 'DUP': code.append('s.append(s[-1])')
            elif k == 'SWAP': code.append('s[-1],s[-2]=s[-2],s[-1]')
            elif k == 'DROP': code.append('s.pop()')
            elif k == 'PRINT': code.append('print(format(s[-1],".15g"))')
            elif k == 'NEG': code.append('s.append(-s.pop())')
            else:
                code.append('b=s.pop(); a=s.pop()')
                if k == 'DIV': code.append('if b == 0: sys.exit("!division_by_zero")')
                code.append(f's.append(a { {"ADD":"+", "SUB":"-", "MUL":"*", "DIV":"/"}[k]} b)')
        code.append('if len(s)!=1: sys.exit("!bad_result_count")')
        code.append('print(format(s[-1],".15g"))')
        return '\n'.join(code)+'\n'
    code = ['#include <stdio.h>', '#include <stdlib.h>', '#include <math.h>', 'int main(void) {',
            f'  double s[{max(8, len(ops)+4)}]; int sp=0; double a=0,b=0; (void)a; (void)b;']
    for op in ops:
        k = op.name.split('.')[-1]
        if k == 'PUSH_F64': code.append(f'  s[sp++]={number_literal(op.args[0], target)};')
        elif k == 'DUP': code.append('  s[sp]=s[sp-1]; sp++;')
        elif k == 'SWAP': code.append('  a=s[sp-1]; s[sp-1]=s[sp-2]; s[sp-2]=a;')
        elif k == 'DROP': code.append('  sp--;')
        elif k == 'PRINT': code.append('  printf("%.15g\\n",s[sp-1]);')
        elif k == 'NEG': code.append('  s[sp-1]=-s[sp-1];')
        else:
            code.append('  b=s[--sp]; a=s[--sp];')
            if k == 'DIV': code.append('  if(b==0){fputs("division by zero\\n",stderr);return 1;}')
            code.append(f'  s[sp++]=a { {"ADD":"+", "SUB":"-", "MUL":"*", "DIV":"/"}[k]} b;')
    code += ['  if(sp!=1){fputs("bad stack height\\n",stderr);return 1;}',
             '  printf("%.15g\\n",s[0]); return 0;', '}']
    return '\n'.join(code)+'\n'

def main(argv=None):
    p = argparse.ArgumentParser(description=SPEC.get('summary','Generated language tool'))
    g = p.add_mutually_exclusive_group()
    g.add_argument('--interpret', action='store_true')
    g.add_argument('--compile', action='store_true')
    g.add_argument('--rewrite', action='store_true')
    g.add_argument('--analyze', action='store_true')
    g.add_argument('--run-bytecode', action='store_true')
    g.add_argument('--parse', action='store_true')
    g.add_argument('--tokens', action='store_true')
    p.add_argument('--target', default=DEFAULT_TARGET, help='program output target, including declarative backends')
    p.add_argument('-o', '--output')
    p.add_argument('file', nargs='?', default='-')
    args = p.parse_args(argv)
    try:
        if args.file == '-': data = sys.stdin.buffer.read()
        else:
            with open(args.file, 'rb') as f: data = f.read()
        text = data if args.run_bytecode else data.decode('utf-8')
        ctx = Context()
        if args.run_bytecode: result = format_value(execute_bytecode(data))+'\n'
        elif args.tokens:
            result = json.dumps([t.__dict__ for t in lex(text)], indent=2)
        elif args.parse:
            root = parse(text)
            def simpl(n): return {'symbol':n.symbol,'children':[simpl(ch) for ch in n.children]}
            result = json.dumps(simpl(root), indent=2)
        elif args.analyze or (MODE == 'analyze' and not args.interpret):
            ctx.analyze(text)
            result = 'analysis OK\n'
        elif args.compile or args.rewrite or (MODE in ('compile', 'rewrite') and not args.interpret): result = ctx.compile(text, args.target)
        else: result = format_value(ctx.interpret(text))+'\n'
        if args.output:
            with open(args.output, 'wb') as f: f.write(result if isinstance(result, bytes) else result.encode('utf-8'))
        elif isinstance(result, bytes): sys.stdout.buffer.write(result)
        else: sys.stdout.write(result)
        return 0
    except UnicodeError as exc:
        print(f'{SPEC["language"]}: LEX: invalid UTF-8: {exc}', file=sys.stderr)
        return 1
    except (LanguageError, ValueError, IndexError, KeyError, ZeroDivisionError, OSError) as exc:
        print(f'{SPEC["language"]}: {exc}', file=sys.stderr)
        return 1

if __name__ == '__main__':
    sys.exit(main())
@@LLS_TEMPLATE_C@@
/* Portable generated runtime and interpreter, C99 / C11 / C++17 / C++20. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <float.h>
#if DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024 || FLT_RADIX != 2
#error Unsupported target: binary64 double is required
#endif
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <locale.h>
#include <setjmp.h>
#ifdef __cplusplus
extern "C" const char lls_spec_json[];
#else
extern const char lls_spec_json[];
#endif
#ifdef __cplusplus
#define LLS_ZERO {}
#else
#define LLS_ZERO {0}
#endif
#define LLS_MODE "@@MODE@@"
#define LLS_TARGET "@@TARGET@@"

typedef char LLS_double_must_be_8_bytes[sizeof(double)==8?1:-1];
typedef struct Json Json;
struct Json {int type; char *text; double num; Json **items; char **keys; int size;};
static char *dup_n(Context *c,const char *s,size_t n){char *p=(char*)ca(c,n+1);memcpy(p,s,n);p[n]=0;return p;}
static void jws(const char **cursor){while(*(*cursor)==' '||*(*cursor)=='\n'||*(*cursor)=='\r'||*(*cursor)=='\t')(*cursor)++;}
static int jhex(int x){if(x>='0'&&x<='9')return x-'0';if(x>='a'&&x<='f')return x-'a'+10;if(x>='A'&&x<='F')return x-'A'+10;return -1;}
static char *jstring(Context *c,const char **cursor){if(*(*cursor)!='"'){fputs("malformed embedded JSON string\n",stderr);exit(2);}(*cursor)++;
 const char *limit=*cursor;while(*limit && *limit!='"'){if(*limit=='\\'&&limit[1])limit++;limit++;}
 size_t cap=(size_t)(limit-*cursor)+1;char *b=(char*)ca(c,cap);size_t n=0;
 while(*(*cursor)&&*(*cursor)!='"'){
  unsigned char c=(unsigned char)*(*cursor)++;
  if(c=='\\'){
   c=(unsigned char)*(*cursor)++;
   if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';else if(c=='b')c='\b';else if(c=='f')c='\f';
   else if(c=='u'){
    unsigned u=0;for(int k=0;k<4;k++){int h=jhex((unsigned char)*(*cursor)++);if(h<0)break;u=u*16+(unsigned)h;}
    if(u<=0x7f){b[n++]=(char)u;continue;}
    if(u<=0x7ff){b[n++]=(char)(0xc0|(u>>6));b[n++]=(char)(0x80|(u&63));continue;}
    b[n++]=(char)(0xe0|(u>>12));b[n++]=(char)(0x80|((u>>6)&63));b[n++]=(char)(0x80|(u&63));continue;
   }
  }
  b[n++]=(char)c;
 }
 if(*(*cursor)=='"')(*cursor)++;
 b[n]=0;return b;
}
static Json *jparse(Context *c,const char **cursor){jws(cursor);Json *j=(Json*)ca(c,sizeof(Json));
 if(*(*cursor)=='{'||*(*cursor)=='['){char end=*(*cursor)++=='{'?'}':']';j->type=end=='}'?4:3;jws(cursor);
  while(*(*cursor)&&*(*cursor)!=end){char *key=NULL;if(j->type==4){key=jstring(c,cursor);jws(cursor);if(*(*cursor)==':')(*cursor)++;}
   Json *value=jparse(c,cursor);j->items=(Json**)crealloc(c,j->items,(size_t)(j->size+1)*sizeof(Json*));j->items[j->size]=value;
   if(j->type==4){j->keys=(char**)crealloc(c,j->keys,(size_t)(j->size+1)*sizeof(char*));j->keys[j->size]=key;}
   j->size++;jws(cursor);if(*(*cursor)==','){(*cursor)++;jws(cursor);}else break;
  }if(*(*cursor)!=end){fputs("malformed embedded JSON object\n",stderr);exit(2);}(*cursor)++;return j;
 }
 if(*(*cursor)=='"'){j->type=2;j->text=jstring(c,cursor);return j;}
 if(*(*cursor)=='-'||isdigit((unsigned char)*(*cursor))){char *end=NULL;j->num=ascii_strtod(c,(*cursor),&end);j->type=1;(*cursor)=end;return j;}
 if(strncmp((*cursor),"true",4)==0){j->type=1;j->num=1;(*cursor)+=4;return j;}
 if(strncmp((*cursor),"false",5)==0){j->type=1;j->num=0;(*cursor)+=5;return j;}
 if(strncmp((*cursor),"null",4)==0){j->type=0;(*cursor)+=4;return j;}
 fputs("malformed embedded JSON\n",stderr);exit(2);
}
static Json *jo(Json *j,const char *key){if(j&&j->type==4)for(int i=0;i<j->size;i++)if(strcmp(j->keys[i],key)==0)return j->items[i];return NULL;}
static Json *ji(Json *j,int i){return j&&j->type==3&&i>=0&&i<j->size?j->items[i]:NULL;}
static const char *js(Json *j){return j&&j->type==2?j->text:"";}
static const char *jg(Json *j,const char *key){return js(jo(j,key));}
static int jlen(Json *j){return j?j->size:0;}
static double jnumber(Json *j){return j&&j->type==1?j->num:0;}
static int jeq(Json *j,const char *str){return strcmp(js(j),str)==0;}


typedef struct Alloc Alloc;struct Alloc {void *ptr;Alloc *next;};
typedef struct Context Context;
static void *ca(Context *c,size_t n);
typedef struct Vec Vec; typedef struct Map Map;typedef struct Op Op;typedef struct Val Val;
struct Val {int tag;int64_t i;double n;const char *s;Vec *vec;Map *map;Op *op;};
enum {VNIL,VNUM,VINT,VBOOL,VSTR,VVEC,VMAP,VFN,VOP,VBUILTIN,VDATA};
struct Vec {int n,cap;Val *a;};
struct Map {int n,cap;const char **keys;Val *values;};
struct Op {const char *name;Vec *args;};
typedef struct Token {const char *name;char *text;Val val;int line,col;} Token;
typedef struct Node Node;struct Node {const char *name;Node **children;Node *parent;int n,prod;Token *tok;Val val;};
struct Context {jmp_buf *oom;FILE *input_file;Json *spec;Alloc *allocs;Map *states,*tables;Node *active;char error[512],rule_stack[512];int line,col,depth,fuel;};
static Json *lls_spec(Context *c){if(!c->spec){const char *cursor=lls_spec_json;c->spec=jparse(c,&cursor);}return c->spec;}
static void allocation_failure(Context *c){
 if(c->oom)longjmp(*c->oom,1);
 /* Internal callers always install an invocation-local recovery point. */
 abort();
}
static void *ca(Context *c,size_t n){
 Alloc *a=(Alloc*)calloc(1,sizeof(Alloc));if(!a)allocation_failure(c);
 a->next=c->allocs;c->allocs=a;
 a->ptr=calloc(1,n?n:1);if(!a->ptr)allocation_failure(c);return a->ptr;
}
static void *crealloc(Context *c,void *ptr,size_t n){
 if(!ptr)return ca(c,n);
 for(Alloc *a=c->allocs;a;a=a->next)if(a->ptr==ptr){void *next=realloc(ptr,n?n:1);if(!next)allocation_failure(c);a->ptr=next;return next;}
 abort();
}
static void clear_ctx(Context *c){
 jmp_buf *handler=c->oom;
 if(c->input_file && c->input_file!=stdin)fclose(c->input_file);
 while(c->allocs){Alloc *p=c->allocs;c->allocs=p->next;free(p->ptr);free(p);}
 memset(c,0,sizeof(*c));c->oom=handler;
}

static Val vnum(double n){Val v=LLS_ZERO;v.tag=VNUM;v.n=n;return v;}
static Val vint(int64_t n){Val v=LLS_ZERO;v.tag=VINT;v.i=n;return v;}
static Val vbool(int n){Val v=LLS_ZERO;v.tag=VBOOL;v.i=n!=0;return v;}
static Val int_binary(Context *c,const char *op,int64_t a,int64_t b){
 int overflow=0;int64_t result=0;
 if(*op=='+'){overflow=(b>0&&a>INT64_MAX-b)||(b<0&&a<INT64_MIN-b);if(!overflow)result=a+b;}
 else if(*op=='-'){overflow=(b<0&&a>INT64_MAX+b)||(b>0&&a<INT64_MIN+b);if(!overflow)result=a-b;}
 else if(*op=='*'){
  if(a>0)overflow=b>0?a>INT64_MAX/b:b<INT64_MIN/a;
  else if(a<0)overflow=b>0?a<INT64_MIN/b:b<0&&a<INT64_MAX/b;
  if(!overflow)result=a*b;
 }else if(*op=='/'){
  if(!b){error(c,"SEM: division by zero");return vint(0);}
  overflow=a==INT64_MIN&&b==-1;if(!overflow)result=a/b;
 }
 if(overflow)error(c,"INTEGER_OVERFLOW: signed 64-bit arithmetic overflow");
 return vint(result);
}

static Val vstr(const char *s){Val v=LLS_ZERO;v.tag=VSTR;v.s=s?s:"";return v;}
static Val vtag(int t,const char *s){Val v=LLS_ZERO;v.tag=t;v.s=s;return v;}
static Val vvec(Vec *x){Val v=LLS_ZERO;v.tag=VVEC;v.vec=x;return v;}
static Val vmap(Map *x){Val v=LLS_ZERO;v.tag=VMAP;v.map=x;return v;}
static Val vop(Op *x){Val v=LLS_ZERO;v.tag=VOP;v.op=x;return v;}
static Vec *new_vec(Context *c){return (Vec*)ca(c,sizeof(Vec));}
static void vec_push(Context *c,Vec *v,Val x){if(v->n==v->cap){int cap=v->cap?v->cap*2:8;Val *a=(Val*)ca(c,(size_t)cap*sizeof(Val));if(v->n)memcpy(a,v->a,(size_t)v->n*sizeof(Val));v->a=a;v->cap=cap;}v->a[v->n++]=x;}
static Map *new_map(Context *c){return (Map*)ca(c,sizeof(Map));}
static int map_find(Map *m,const char *key){if(!m)return -1;for(int i=0;i<m->n;i++)if(strcmp(m->keys[i],key)==0)return i;return -1;}
static int map_has(Map *m,const char *key){return map_find(m,key)>=0;}
static Val map_get(Map *m,const char *key){int i=map_find(m,key);Val v=LLS_ZERO;return i>=0?m->values[i]:v;}
static void map_put(Context *c,Map *m,const char *key,Val v){int k=map_find(m,key);if(k>=0){m->values[k]=v;return;}if(m->n==m->cap){int cap=m->cap?m->cap*2:8;const char **keys=(const char**)ca(c,(size_t)cap*sizeof(char*));Val *vals=(Val*)ca(c,(size_t)cap*sizeof(Val));if(m->n){memcpy(keys,m->keys,(size_t)m->n*sizeof(char*));memcpy(vals,m->values,(size_t)m->n*sizeof(Val));}m->keys=keys;m->values=vals;m->cap=cap;}m->keys[m->n]=key;m->values[m->n++]=v;}
static int64_t string_length(const char *text){int64_t n=0;for(;*text;text++)if(((unsigned char)*text&0xc0)!=0x80)n++;return n;}
static int truth(Val v){if(v.tag==VNIL)return 0;if(v.tag==VNUM)return v.n!=0;if(v.tag==VINT||v.tag==VBOOL)return v.i!=0;if(v.tag==VSTR)return v.s&&*v.s;if(v.tag==VVEC)return v.vec&&v.vec->n>0;return 1;}
static const char *val_string(Context *c,Val v){if(v.tag==VSTR||v.tag==VFN||v.tag==VBUILTIN)return v.s?v.s:"";if(v.tag==VOP)return v.op?v.op->name:"";char buf[64];if(v.tag==VBOOL)return v.i?"true":"false";if(v.tag==VINT)snprintf(buf,sizeof buf,"%" PRId64,v.i);else if(v.tag==VNUM)ascii_format(buf,sizeof buf,"%.15g",v.n);else snprintf(buf,sizeof buf,"null");char *p=(char*)ca(c,strlen(buf)+1);strcpy(p,buf);return p;}
static Token *first_tok(Node *n){if(!n)return NULL;if(n->tok)return n->tok;for(int i=0;i<n->n;i++){Token *t=first_tok(n->children[i]);if(t)return t;}return NULL;}
static void error(Context *c,const char *fmt,...){if(c->error[0])return;va_list ap;va_start(ap,fmt);vsnprintf(c->error,sizeof(c->error),fmt,ap);va_end(ap);Token *t=first_tok(c->active);if(t){c->line=t->line;c->col=t->col;}
 const char *frames[128];int count=0;for(Node *n=c->active;n&&count<128;n=n->parent)if(n->prod>=0&&!jo(jo(lls_spec(c),"internal"),n->name))frames[count++]=n->name;
 for(int i=count-1;i>=0;i--){size_t used=strlen(c->rule_stack);snprintf(c->rule_stack+used,sizeof(c->rule_stack)-used,"%s%s",used?" -> ":"",frames[i]);}
}
static void throw_error(Context *c,const char *name,Vec *args){Json *exceptions=jo(lls_spec(c),"exceptions");const char *n=*name=='!'?name+1:name;
 for(int i=0;i<jlen(exceptions);i++){Json *ex=ji(exceptions,i);if(strcmp(jg(ex,"name"),n))continue;
  char message[400];snprintf(message,sizeof message,"%s",jg(ex,"message"));
  char *params=dup_n(c,jg(ex,"params"),strlen(jg(ex,"params")));char *pos=params;
  for(int j=0;j<(args?args->n:0);j++){char *next=strchr(pos,',');if(next)*next=0;char *colon=strchr(pos,':');if(colon)*colon=0;while(*pos==' ')pos++;
   char pat[96];snprintf(pat,sizeof pat,"{%s}",pos);char replace[400];snprintf(replace,sizeof replace,"%s",val_string(c,args->a[j]));char out[400]={0};char *where=strstr(message,pat);
   if(where){size_t before=(size_t)(where-message);if(before>=sizeof out)before=sizeof out-1;memcpy(out,message,before);strncat(out,replace,sizeof out-strlen(out)-1);strncat(out,where+strlen(pat),sizeof out-strlen(out)-1);snprintf(message,sizeof message,"%s",out);}
   if(next)pos=next+1;else break;
  }error(c,"!%s: %s",n,message);return;
 }
 error(c,"!%s",n);
}

/* Generated DFA table-driven lexer. No regex runtime dependency. */
static int valid_utf8(const unsigned char *s){while(*s){unsigned n=0;
  if(*s<0x80){s++;continue;}
  if((*s&0xe0)==0xc0 && *s>=0xc2)n=2;
  else if((*s&0xf0)==0xe0)n=3;
  else if((*s&0xf8)==0xf0 && *s<=0xf4)n=4;
  else return 0;
  for(unsigned i=1;i<n;i++){if(s[i]==0||(s[i]&0xc0)!=0x80)return 0;}
  if(n==3 && ((s[0]==0xe0&&s[1]<0xa0)||(s[0]==0xed&&s[1]>=0xa0)))return 0;
  if(n==4 && ((s[0]==0xf0&&s[1]<0x90)||(s[0]==0xf4&&s[1]>=0x90)))return 0;
  s+=n;
 }return 1;}
static Vec *lex(Context *c,const char *src){Vec *v=new_vec(c);
 if(!valid_utf8((const unsigned char*)src)){error(c,"LEX: invalid UTF-8");return v;}
 Json *dfa=jo(lls_spec(c),"lexdfa"),*rows=jo(dfa,"states"),*rules=jo(dfa,"rules");
 size_t pos=0,total=strlen(src);int line=1,col=1;
 while(pos<total&&!c->error[0]){
  int state=0,best_rule=-1;size_t length=0,best_length=0;
  while(1){Json *row=ji(rows,state),*accepts=jo(row,"accept");
   for(int i=0;i<jlen(accepts);i++){
    int ruleid=(int)jnumber(ji(accepts,i));Json *rule=ji(rules,ruleid);
    int bol=jnumber(jo(rule,"bol"))!=0,eol=jnumber(jo(rule,"eol"))!=0;
    if(bol && pos>0 && src[pos-1]!='\n')continue;
    if(eol && pos+length<total && src[pos+length]!='\n')continue;
    if(length>best_length || (length==best_length && best_rule>=0 && ruleid<best_rule)){
     best_rule=ruleid;best_length=length;
    }
   }
   if(pos+length>=total)break;
   int ch=(unsigned char)src[pos+length];int to=-1;Json *trans=jo(row,"trans");
   for(int i=0;i<jlen(trans);i++){Json *r=ji(trans,i);int lo=(int)jnumber(ji(r,0)),hi=(int)jnumber(ji(r,1));
    if(ch>=lo && ch<=hi){to=(int)jnumber(ji(r,2));break;}
   }
   if(to<0)break;
   state=to;length++;
  }
  if(best_rule<0){c->line=line;c->col=col;error(c,"LEX: unexpected byte 0x%02x at %d:%d",(unsigned char)src[pos],line,col);break;}
  if(pos+best_length<total && (((unsigned char)src[pos+best_length]&0xc0)==0x80)){error(c,"LEX: lexer split a UTF-8 sequence");break;}
 c->line=line;c->col=col;Json *r=ji(rules,best_rule);
  if(jnumber(jo(r,"skip"))==0){Token *t=(Token*)ca(c,sizeof(Token));t->name=jg(r,"name");
   t->text=(char*)ca(c,best_length+1);memcpy(t->text,src+pos,best_length);t->line=line;t->col=col;
   const char *type=jg(r,"type");
   if(strcmp(type,"Float64")==0||strcmp(type,"Float32")==0||strcmp(type,"Int")==0||strcmp(type,"Int64")==0){
    char *end=NULL;errno=0;if(!strncmp(type,"Int",3)){int64_t x=(int64_t)strtoll(t->text,&end,10);if(errno==ERANGE||!end||*end)error(c,"INTEGER_OVERFLOW: invalid signed 64-bit literal");t->val=vint(x);}else{double x=ascii_strtod(c,t->text,&end);if(!isfinite(x)||!end||*end)error(c,"LEX: malformed numeric literal");t->val=vnum(x);}
   }else if(!strcmp(type,"Bool")){if(strcmp(t->text,"true")&&strcmp(t->text,"false"))error(c,"LEX: invalid Bool literal");t->val=vbool(!strcmp(t->text,"true"));}
   else t->val=vstr(t->text);
   Val x=LLS_ZERO;x.tag=VBUILTIN;x.s=(const char*)t;vec_push(c,v,x);
  }
  for(size_t i=0;i<best_length;i++){
   unsigned char ch=(unsigned char)src[pos+i];
   if(ch=='\n'){line++;col=1;}else if((ch&0xc0)!=0x80)col++;
  }
  pos+=best_length;
 }
 for(int k=0;k<2;k++){Token *t=(Token*)ca(c,sizeof(Token));t->name=k==0?"EOF":"$";t->text=(char*)"";t->line=line;t->col=col;
  Val x=LLS_ZERO;x.tag=VBUILTIN;x.s=(const char*)t;vec_push(c,v,x);
 }
 return v;
}
static Node *new_node(Context *c,const char *name,int prod,Node **kids,int n,Token *tok){Node *x=(Node*)ca(c,sizeof(Node));x->name=name;x->prod=prod;x->children=kids;for(int i=0;i<n;i++)kids[i]->parent=x;x->n=n;x->tok=tok;if(tok)x->val=tok->val;else for(int i=0;i<n;i++)if(kids[i]->tok){x->tok=kids[i]->tok;break;}return x;}
static Node *parse(Context *c,const char *source){Vec *tokens=lex(c,source);if(c->error[0])return NULL;
 Json *a=jo(lls_spec(c),"actions"),*g=jo(lls_spec(c),"gotos"),*prods=jo(lls_spec(c),"productions");
 Vec *states=new_vec(c);Vec *nodes=new_vec(c);vec_push(c,states,vnum(0));int ti=0;
 for(int steps=0;steps<2000000 && !c->error[0];steps++){
  if(ti>=tokens->n){error(c,"PARSE: unexpected end");break;}
  Token *tok=(Token*)tokens->a[ti].s;c->line=tok->line;c->col=tok->col;int state=(int)states->a[states->n-1].n;
  Json *entry=jo(ji(a,state),tok->name);
  if(!entry){error(c,"PARSE: unexpected %s '%s' at %d:%d",tok->name,tok->text,tok->line,tok->col);break;}
  const char *action=js(ji(entry,0));int arg=(int)jnumber(ji(entry,1));
  if(jnumber(jo(lls_spec(c),"trace_parser")))fprintf(stderr,"parser: state=%d token=%s action=%s:%d\n",state,tok->name,action,arg);
  if(strcmp(action,"s")==0){Node *n=new_node(c,tok->name,-1,NULL,0,tok);Val x=LLS_ZERO;x.s=(const char*)n;vec_push(c,nodes,x);vec_push(c,states,vnum(arg));ti++;}
  else if(strcmp(action,"r")==0){Json *prod=ji(prods,arg);int count=jlen(jo(prod,"rhs"));
   if(count>=states->n || count>nodes->n){error(c,"PARSE: corrupt reduction table");break;}
   Node **children=(Node**)ca(c,(size_t)(count+1)*sizeof(Node*));for(int k=0;k<count;k++)children[k]=(Node*)nodes->a[nodes->n-count+k].s;
   states->n-=count;nodes->n-=count;
   const char *lhs=jg(prod,"lhs");Node *node=new_node(c,lhs,arg,children,count,NULL);
   Json *to=jo(ji(g,(int)states->a[states->n-1].n),lhs);
   if(!to){error(c,"PARSE: missing goto table entry");break;}
   Val x=LLS_ZERO;x.s=(const char*)node;vec_push(c,nodes,x);vec_push(c,states,vnum(jnumber(to)));
  }
  else if(strcmp(action,"a")==0){return (Node*)nodes->a[nodes->n-1].s;}
  else{error(c,"PARSE: invalid action");break;}
 }
 if(!c->error[0])error(c,"PARSE: budget exceeded");
 return NULL;
}

static Val eval(Context *,Json *,Map *,Map *);
static int exec_block(Context *,Json *,Map *,Map *,Val *);
static int eq_val(Val a,Val b){if((a.tag==VINT||a.tag==VBOOL)&&(b.tag==VINT||b.tag==VBOOL))return a.i==b.i;if((a.tag==VNUM||a.tag==VINT||a.tag==VBOOL)&&(b.tag==VNUM||b.tag==VINT||b.tag==VBOOL))return number(a)==number(b);if(a.tag==VNIL||b.tag==VNIL)return a.tag==b.tag;
 if(a.tag!=b.tag)return 0;
 if(a.tag==VVEC){if(a.vec->n!=b.vec->n)return 0;for(int i=0;i<a.vec->n;i++)if(!eq_val(a.vec->a[i],b.vec->a[i]))return 0;return 1;}
 if(a.tag==VMAP){if(a.map->n!=b.map->n)return 0;for(int i=0;i<a.map->n;i++){if(!map_has(b.map,a.map->keys[i])||!eq_val(a.map->values[i],map_get(b.map,a.map->keys[i])))return 0;}return 1;}
 if(a.tag==VOP||a.tag==VDATA){if(strcmp(a.op->name,b.op->name))return 0;return eq_val(vvec(a.op->args),vvec(b.op->args));}
 return a.tag==b.tag&&strcmp(a.s?a.s:"",b.s?b.s:"")==0;}
static double number(Val v){return v.tag==VNUM?v.n:(v.tag==VINT||v.tag==VBOOL)?(double)v.i:0;}
@@INTEROP_C@@

static Val eval_call(Context *c,Val callee,Vec *args){Val nil=LLS_ZERO;const char *name=callee.s?callee.s:"";
 if(callee.tag==VOP){Op *p=(Op*)ca(c,sizeof(Op));p->name=callee.op->name;p->args=args;return vop(p);}
 if(callee.tag==VFN && name[0]=='%'){
  Json *op=jo(jo(lls_spec(c),"type_ops"),name);
  if(!op||args->n!=jlen(jo(op,"params"))){error(c,"SEM: wrong arity for %s",name);return nil;}
  const char *action=jg(op,"action");if(!strcmp(action,"unit"))return nil;
  const char *tag=jg(op,"tag"),*owner=jg(op,"owner");
  if(!strcmp(action,"make")){
   Op *p=(Op*)ca(c,sizeof(Op));p->name=tag;p->args=args;
   Val result=vop(p);result.tag=VDATA;result.s=owner;return result;
  }
  Val data=args->a[0];if(data.tag!=VDATA||strcmp(data.s,owner)){error(c,"SEM: expected a value of %s",owner);return nil;}
  int match=!strcmp(data.op->name,tag);
  if(!strcmp(action,"is"))return vbool(match);
  if(!match){error(c,"SEM: expected %s, got %s",tag,data.op->name);return nil;}
  return data.op->args->a[(int)jnumber(jo(op,"index"))];
 }
 if(callee.tag==VFN){Json *functions=jo(lls_spec(c),"functions");for(int i=0;i<jlen(functions);i++){Json *f=ji(functions,i);if(strcmp(jg(f,"name"),name))continue;
   if(*jg(f,"foreign"))return interop_call(c,f,args);
   Map *env=new_map(c),*caps=new_map(c);Json *params=jo(f,"parameters");
   if(args->n!=jlen(params)){error(c,"SEM: wrong arity for %s",name);return nil;}
   for(int x=0;x<args->n;x++)map_put(c,env,jg(ji(params,x),"name"),args->a[x]);
   Val result=LLS_ZERO;if(c->depth>=128){error(c,"RESOURCE: function recursion limit exceeded");return result;}c->depth++;exec_block(c,jo(f,"code"),env,caps,&result);c->depth--;return result;
  }error(c,"SEM: unknown function %s",name);return nil;
 }
 if(strcmp(name,"reverse")==0){Vec *out=new_vec(c);if(args->n && args->a[0].tag==VVEC)for(int i=args->a[0].vec->n-1;i>=0;i--)vec_push(c,out,args->a[0].vec->a[i]);return vvec(out);}
 if(strcmp(name,"is_finite")==0){return vbool(args->n==1 && isfinite(number(args->a[0])));}
 if(strcmp(name,"abs")==0){Val a=args->a[0];return a.tag==VINT?(a.i<0?int_binary(c,"-",0,a.i):a):vnum(fabs(number(a)));}
 if(strcmp(name,"min")==0){return args->n>1 && (args->a[0].tag==VINT&&args->a[1].tag==VINT?args->a[1].i<args->a[0].i:number(args->a[1])<number(args->a[0]))?args->a[1]:args->a[0];}
 if(strcmp(name,"max")==0){return args->n>1 && (args->a[0].tag==VINT&&args->a[1].tag==VINT?args->a[1].i>args->a[0].i:number(args->a[1])>number(args->a[0]))?args->a[1]:args->a[0];}
 if(strcmp(name,"len")==0){Val a=args->n?args->a[0]:nil;return vint(a.tag==VVEC?a.vec->n:a.tag==VMAP?a.map->n:a.tag==VSTR?string_length(a.s):0);}
 if(strcmp(name,"pop")==0||strcmp(name,"top")==0){Vec *v=args->n?args->a[0].vec:NULL;if(!v||v->n<1){error(c,"SEM: empty stack");return nil;}return strcmp(name,"pop")==0?v->a[--v->n]:v->a[v->n-1];}
 if(strcmp(name,"push")==0||strcmp(name,"emit")==0){if(args->n!=2||args->a[0].tag!=VVEC){error(c,"SEM: expected vector argument");return nil;}vec_push(c,args->a[0].vec,args->a[1]);return nil;}
 if(strcmp(name,"print")==0){if(args->n)printf("%s\n",val_string(c,args->a[0]));return nil;}
 if(strcmp(name,"final_stack_height")==0)return vint(verify_ops(c,args->n && args->a[0].tag==VVEC?args->a[0].vec:NULL));
 error(c,"SEM: unknown callable %s",name);return nil;
}
static Val eval(Context *c,Json *e,Map *env,Map *caps){Val nil=LLS_ZERO;if(!e||c->error[0])return nil;const char *kind=jg(e,"k");
 if(strcmp(kind,"num")==0){if(jeq(jo(e,"type"),"Int"))return vint((int64_t)strtoll(jg(e,"literal"),NULL,10));return vnum(jnumber(jo(e,"v")));}
 if(strcmp(kind,"str")==0)return vstr(jg(e,"v"));
 if(strcmp(kind,"list")==0){Vec *v=new_vec(c);Json *items=jo(e,"args");for(int i=0;i<jlen(items);i++)vec_push(c,v,eval(c,ji(items,i),env,caps));return vvec(v);}
 if(strcmp(kind,"name")==0){const char *name=jg(e,"v");if(name[0]=='@')return map_get(c->states,name+1);
  if(name[0]=='#')return map_get(c->tables,name+1);
  if(name[0]=='$'){if(!map_has(caps,name+1))error(c,"SEM: unbound capture %s",name);return map_get(caps,name+1);}
   if(name[0]=='&')return vtag(VFN,name+1);
   if(name[0]=='%')return vtag(VFN,name);
  if(strcmp(name,"true")==0)return vbool(1);
  if(strcmp(name,"false")==0)return vbool(0);
  if(strcmp(name,"nil")==0||strcmp(name,"None")==0)return nil;
  if(jo(jo(lls_spec(c),"op_by_name"),name)){Op *op=(Op*)ca(c,sizeof(Op));op->name=name;op->args=new_vec(c);return vop(op);}
  if(map_has(env,name))return map_get(env,name);
  Json *functions=jo(lls_spec(c),"functions");
  for(int i=0;i<jlen(functions);i++)if(!strcmp(jg(ji(functions,i),"name"),name))return vtag(VFN,name);
  return vtag(VBUILTIN,name);
 }
 if(strcmp(kind,"un")==0){Val a=eval(c,jo(e,"a"),env,caps);if(strcmp(jg(e,"v"),"-")==0){if(a.tag==VINT)return int_binary(c,"-",0,a.i);return vnum(-number(a));}return vbool(!truth(a));}
 if(strcmp(kind,"index")==0){Val a=eval(c,jo(e,"a"),env,caps),b=eval(c,jo(e,"b"),env,caps);
  if(a.tag==VMAP){if(!map_has(a.map,val_string(c,b)))error(c,"SEM: missing table key '%s'",val_string(c,b));return map_get(a.map,val_string(c,b));}
  if(a.tag==VVEC){int i=(int)number(b);if(i<0||i>=a.vec->n){error(c,"SEM: index out of range");return nil;}return a.vec->a[i];}
  error(c,"SEM: invalid index expression");return nil;
 }
 if(strcmp(kind,"bin")==0){const char *op=jg(e,"v");Val a=eval(c,jo(e,"a"),env,caps);
  if(strcmp(op,"&&")==0)return vbool(truth(a)&&truth(a=eval(c,jo(e,"b"),env,caps)));
  if(strcmp(op,"||")==0)return vbool(truth(a)||truth(a=eval(c,jo(e,"b"),env,caps)));
  Val b=eval(c,jo(e,"b"),env,caps);double x=number(a),y=number(b);
  if(jeq(jo(e,"type"),"Int")||(jeq(jo(e,"type"),"Any")&&a.tag==VINT&&b.tag==VINT))return int_binary(c,op,a.i,b.i);
  if(strcmp(op,"+")==0)return vnum(x+y);
  if(strcmp(op,"-")==0)return vnum(x-y);
  if(strcmp(op,"*")==0)return vnum(x*y);
  if(strcmp(op,"/")==0){if(y==0){error(c,"SEM: division by zero");return nil;}return vnum(x/y);}
  if(strcmp(op,"==")==0)return vbool(eq_val(a,b));
  if(strcmp(op,"!=")==0)return vbool(!eq_val(a,b));
  if(strcmp(op,">=")==0)return vbool(a.tag==VINT&&b.tag==VINT?a.i>=b.i:x>=y);
  if(strcmp(op,"<=")==0)return vbool(a.tag==VINT&&b.tag==VINT?a.i<=b.i:x<=y);
  if(strcmp(op,">")==0)return vbool(a.tag==VINT&&b.tag==VINT?a.i>b.i:x>y);
  if(strcmp(op,"<")==0)return vbool(a.tag==VINT&&b.tag==VINT?a.i<b.i:x<y);
  if(strcmp(op,"in")==0){if(b.tag==VMAP)return vbool(map_has(b.map,val_string(c,a)));if(b.tag==VVEC){for(int i=0;i<b.vec->n;i++)if(eq_val(a,b.vec->a[i]))return vbool(1);}return vbool(0);}
  error(c,"SEM: invalid binary operator %s",op);return nil;
 }
 if(strcmp(kind,"call")==0){Val fn=eval(c,jo(e,"a"),env,caps);Vec *args=new_vec(c);Json *ja=jo(e,"args");for(int i=0;i<jlen(ja);i++)vec_push(c,args,eval(c,ji(ja,i),env,caps));
  return c->error[0]?nil:eval_call(c,fn,args);
 }
 error(c,"SEM: bad expression %s",kind);return nil;
}
static int exec_block(Context *c,Json *code,Map *env,Map *caps,Val *result){for(int i=0;i<jlen(code)&&!c->error[0];i++){
 if(--c->fuel<0){error(c,"RESOURCE: action instruction budget exhausted");return 0;}
 Json *stmt=ji(code,i);const char *k=jg(stmt,"k");
 if(strcmp(k,"on")==0){if(map_has(caps,jg(stmt,"name")))if(exec_block(c,jo(stmt,"body"),env,caps,result))return 1;}
 else if(strcmp(k,"require")==0){if(!truth(eval(c,jo(stmt,"cond"),env,caps))){Json *ja=jo(stmt,"args");Vec *args=new_vec(c);for(int t=0;t<jlen(ja);t++)vec_push(c,args,eval(c,ji(ja,t),env,caps));throw_error(c,jg(stmt,"name"),args);}}
 else if(strcmp(k,"throw")==0){Json *ja=jo(stmt,"args");Vec *args=new_vec(c);for(int t=0;t<jlen(ja);t++)vec_push(c,args,eval(c,ji(ja,t),env,caps));throw_error(c,jg(stmt,"name"),args);}
 else if(strcmp(k,"let")==0){map_put(c,env,jg(stmt,"name"),eval(c,jo(stmt,"expr"),env,caps));}
 else if(strcmp(k,"grab")==0){Val key=eval(c,jo(stmt,"key"),env,caps);const char *table=jg(stmt,"table");Map *m=map_get(c->tables,table+1).map;
  if(!m||!map_has(m,val_string(c,key)))error(c,"SEM: key '%s' missing in %s",val_string(c,key),table);
  else map_put(c,env,jg(stmt,"var"),map_get(m,val_string(c,key)));
 }
 else if(strcmp(k,"assign")==0){Json *lhs=jo(stmt,"lhs");Val rhs=eval(c,jo(stmt,"rhs"),env,caps);
  if(jeq(jo(lhs,"k"),"index")){Val a=eval(c,jo(lhs,"a"),env,caps),b=eval(c,jo(lhs,"b"),env,caps);
   if(a.tag==VMAP)map_put(c,a.map,val_string(c,b),rhs);
   else if(a.tag==VVEC){int x=(int)number(b);if(x>=0&&x<a.vec->n)a.vec->a[x]=rhs;else error(c,"SEM: invalid vector index");}
   else error(c,"SEM: assignment requires map or vector");
  }else{const char *name=jg(lhs,"v");if(*name=='@')map_put(c,c->states,name+1,rhs);else map_put(c,env,name,rhs);}
 }
 else if(strcmp(k,"exec")==0){(void)eval(c,jo(stmt,"expr"),env,caps);}
 else if(strcmp(k,"return")==0){*result=eval(c,jo(stmt,"expr"),env,caps);return 1;}
 else if(strcmp(k,"while")==0){while(!c->error[0]&&truth(eval(c,jo(stmt,"cond"),env,caps))){
  if(--c->fuel<0){error(c,"RESOURCE: action instruction budget exhausted");return 0;}
  if(exec_block(c,jo(stmt,"body"),env,caps,result))return 1;
 }}
 else if(strcmp(k,"for")==0){Val items=eval(c,jo(stmt,"items"),env,caps);if(items.tag!=VVEC){error(c,"SEM: for requires vector");return 0;}
  Vec *snapshot=new_vec(c);for(int j=0;j<items.vec->n;j++)vec_push(c,snapshot,items.vec->a[j]);
  for(int j=0;j<snapshot->n&&!c->error[0];j++){if(--c->fuel<0){error(c,"RESOURCE: action instruction budget exhausted");return 0;}
   map_put(c,env,jg(stmt,"var"),snapshot->a[j]);if(exec_block(c,jo(stmt,"body"),env,caps,result))return 1;
  }
 }
 else if(strcmp(k,"if")==0){if(exec_block(c,jo(stmt,truth(eval(c,jo(stmt,"cond"),env,caps))?"yes":"no"),env,caps,result))return 1;}
 else error(c,"SEM: unknown statement %s",k);
 }return 0;
}
static void reset(Context *c){clear_ctx(c);c->fuel=1000000;c->states=new_map(c);c->tables=new_map(c);
 Json *s=lls_spec(c),*states=jo(s,"states"),*tables=jo(s,"tables");
 Map *initial_env=new_map(c),*initial_caps=new_map(c);
 for(int i=0;i<jlen(states);i++){Json *state=ji(states,i);const char *name=jg(state,"name");
  map_put(c,c->states,name,eval(c,jo(state,"initial_expr"),initial_env,initial_caps));}
 for(int i=0;i<jlen(tables);i++){const char *name=jg(ji(tables,i),"name");map_put(c,c->tables,name,vmap(new_map(c)));}
 Map *env=new_map(c),*caps=new_map(c);Val r=LLS_ZERO;exec_block(c,jo(s,"init_code"),env,caps,&r);
}
static void walk_node(Context *c,Node *node,const char *facet){if(!node||c->error[0])return;Node *old=c->active;c->active=node;
 if(node->prod>=0&&!c->error[0]){Json *spec=lls_spec(c),*prod=ji(jo(spec,"productions"),node->prod);Json *rules=jo(spec,"rules");
  for(int i=0;i<jlen(rules);i++){Json *rule=ji(rules,i);if(strcmp(jg(rule,"name"),jg(prod,"lhs")))continue;
    Map *caps=new_map(c),*env=new_map(c);Json *labels=jo(prod,"labels");
    for(int j=0;j<node->n;j++){const char *label=js(ji(labels,j));if(*label)map_put(c,caps,label,node->children[j]->val);}
    Val r=LLS_ZERO;exec_block(c,jo(jo(rule,"code"),facet),env,caps,&r);node->val=r;break;
  }
  Json *kind=jo(jo(spec,"internal"),jg(prod,"lhs"));
  if(kind){const char *k=js(kind);
   if(strcmp(k,"*")==0||strcmp(k,"+")==0||strcmp(k,"sep")==0){
    Vec *items=new_vec(c);
    if(node->n==1)vec_push(c,items,node->children[0]->val);
    else if(node->n>1){Val first=node->children[0]->val;
     if(first.tag==VVEC)items=first.vec;
     vec_push(c,items,node->children[node->n-1]->val);
    }
    node->val=vvec(items);
   }else if(strcmp(k,"?")==0){if(node->n)node->val=node->children[0]->val;}
   else {if(node->n==1)node->val=node->children[0]->val;
    else {Vec *items=new_vec(c);for(int x=0;x<node->n;x++)vec_push(c,items,node->children[x]->val);node->val=vvec(items);}
   }
  }
 }
 c->active=old;
}
static void walk(Context *c,Node *root,const char *facet){
 Vec *pending=new_vec(c);Val entry=LLS_ZERO;entry.s=(const char*)root;vec_push(c,pending,entry);
 while(pending->n&&!c->error[0]){Val item=pending->a[--pending->n];Node *node=(Node*)item.s;
  if(item.n){walk_node(c,node,facet);continue;}
  item.n=1;vec_push(c,pending,item);
  for(int i=node->n-1;i>=0;i--){Val child=LLS_ZERO;child.s=(const char*)node->children[i];vec_push(c,pending,child);}
 }
}
static Val run(Context *c,const char *source,const char *facet){Node *root=parse(c,source);Val r=LLS_ZERO;if(root&&!c->error[0]){walk(c,root,facet);r=root->val;}return r;}

/* Declarative, bounded, stack-effect-preserving StackIR rewrite engine. */
static int is_identifier(const char *s){if(!s || !(*s=='_' || isalpha((unsigned char)*s)))return 0;
 for(s++;*s;s++){if(!(*s=='_'||isalnum((unsigned char)*s)))return 0;}return 1;}
static int op_effect(Json *decl){return (int)jnumber(jo(decl,"push"))-(int)jnumber(jo(decl,"pop"));}
static double ascii_strtod(Context *c,const char *text,char **end){
 const char *p=text;while(*p=='+'||*p=='-'||*p=='.'||*p=='e'||*p=='E'||(*p>='0'&&*p<='9'))p++;
 size_t n=(size_t)(p-text);const char *decimal=localeconv()->decimal_point;size_t dn=strlen(decimal);
 char *local=(char*)ca(c,n*(dn+1)+1);size_t out=0;
 for(size_t i=0;i<n;i++){if(text[i]=='.'){memcpy(local+out,decimal,dn);out+=dn;}else local[out++]=text[i];}
 char *tail=NULL;double value=strtod(local,&tail);
 if(end)*end=(char*)(tail==local+out?p:text);
 return value;
}
static void ascii_format(char *dest,size_t size,const char *format,double value){
 char local[128];snprintf(local,sizeof(local),format,value);
 const char *decimal=localeconv()->decimal_point;char *point=strstr(local,decimal);
 if(point && strcmp(decimal,".")){size_t n=strlen(decimal);*point='.';memmove(point+1,point+n,strlen(point+n)+1);}
 snprintf(dest,size,"%s",local);
}
static const char *number_literal(Context *c,double value,int python){
 if(!isfinite(value)){if(isnan(value))return python?"float('nan')":"NAN";return value<0?(python?"float('-inf')":"(-INFINITY)"):(python?"float('inf')":"INFINITY");}
 char buf[128];ascii_format(buf,sizeof buf,"%.17g",value);
 if(!strchr(buf,'.')&&!strchr(buf,'e')&&!strchr(buf,'E'))strcat(buf,".0");
 char *result=(char*)ca(c,strlen(buf)+1);strcpy(result,buf);return result;
}
static const char *quoted_string(Context *c,const char *text,int python){
 size_t n=strlen(text);char *out=(char*)ca(c,n*4+3);size_t j=0;out[j++]='"';
 for(size_t i=0;i<n;i++){unsigned char ch=(unsigned char)text[i];
  if(ch=='"'||ch=='\\'){out[j++]='\\';out[j++]=(char)ch;}
  else if(ch<32||(!python&&ch>=127)){out[j++]='\\';out[j++]=(char)('0'+(ch>>6));out[j++]=(char)('0'+((ch>>3)&7));out[j++]=(char)('0'+(ch&7));}
  else out[j++]=(char)ch;
 }
 out[j++]='"';out[j]=0;return out;
}

static int verify_ops(Context *c,Vec *ops){
 Vec *stack=new_vec(c);
 if(!ops){error(c,"IR: expected code vector");return -1;}
 for(int i=0;i<ops->n&&!c->error[0];i++){
  if(ops->a[i].tag!=VOP){error(c,"IR: expected operation");return -1;}
  Op *op=ops->a[i].op;Json *decl=jo(jo(lls_spec(c),"op_by_name"),op->name);
  if(!decl){error(c,"IR: unknown opcode %s",op->name);return -1;}
  int count=0;const char *params=jg(decl,"params");
  while(*params){const char *colon=strchr(params,':');if(!colon)break;const char *type=colon+1;while(*type==' ')type++;
   if(count>=op->args->n){error(c,"IR: wrong operand count for %s",op->name);return -1;}
   Val value=op->args->a[count];int valid=!strncmp(type,"Float64",7)?(value.tag==VNUM||value.tag==VINT):!strncmp(type,"Int",3)?value.tag==VINT:!strncmp(type,"String",6)?value.tag==VSTR:!strncmp(type,"Bool",4)?value.tag==VBOOL:0;
   if(!valid){error(c,"IR: invalid operand type for %s",op->name);return -1;}
   count++;params=strchr(colon,',');if(!params)break;params++;
  }
  if(count!=op->args->n){error(c,"IR: wrong operand count for %s",op->name);return -1;}
  Json *in=jo(decl,"inputs"),*out=jo(decl,"outputs");
  if(stack->n<jlen(in)){Vec *args=new_vec(c);vec_push(c,args,vstr(op->name));throw_error(c,"!stack_underflow",args);return -1;}
  int base=stack->n-jlen(in);
  for(int j=0;j<jlen(in);j++)if(strcmp(stack->a[base+j].s,js(ji(in,j)))){error(c,"IR: stack type mismatch at %s",op->name);return -1;}
  stack->n=base;
  for(int j=0;j<jlen(out);j++)vec_push(c,stack,vstr(js(ji(out,j))));
 }
 return c->error[0]?-1:stack->n;
}

static int same_ops(Vec *a,Vec *b){
 if(a->n!=b->n)return 0;
 for(int i=0;i<a->n;i++){Op *x=a->a[i].op,*y=b->a[i].op;if(strcmp(x->name,y->name)||x->args->n!=y->args->n)return 0;
  for(int j=0;j<x->args->n;j++){Val u=x->args->a[j],v=y->args->a[j];
   if(u.tag==VNUM&&v.tag==VNUM){if(memcmp(&u.n,&v.n,sizeof(double)))return 0;}else if(!eq_val(u,v))return 0;
  }
 }
 return 1;
}
static void rewrite_ops(Context *c,Vec *ops){if(verify_ops(c,ops)<0)return;Json *rules=jo(lls_spec(c),"rewrites_compiled");if(jlen(rules)==0||!jnumber(jo(lls_spec(c),"rewrite_enabled")))return;
 Vec *seen=new_vec(c);
 int fuel=(int)jnumber(jo(lls_spec(c),"rewrite_fuel"));
 for(int iteration=0;iteration<=(int)jnumber(jo(lls_spec(c),"rewrite_fuel")) && !c->error[0];iteration++){
  for(int si=0;si<seen->n;si++)if(same_ops(seen->a[si].vec,ops)){error(c,"REWRITE: rewrite cycle detected");return;}
  Vec *snapshot=new_vec(c);*snapshot=*ops;vec_push(c,seen,vvec(snapshot));
  int changed=0;
  for(int ri=0;ri<jlen(rules)&&!c->error[0];ri++){
   Json *rule=ji(rules,ri),*match=jo(rule,"match"),*replace=jo(rule,"replace");
   if(!jeq(jo(rule,"phase"),"optimize"))continue;
   int count=jlen(match);if(count<1){error(c,"REWRITE: empty pattern");return;}
   for(int i=0;i+count<=ops->n&&!c->error[0];){
    Map *env=new_map(c),*caps=new_map(c);
    const char *fp=jg(jo(lls_spec(c),"options"),"fp_mode");
    map_put(c,env,"fp_mode",vstr(*fp?fp:"strict"));
    int valid=1;
    for(int j=0;j<count&&valid;j++){
     Val vv=ops->a[i+j];if(vv.tag!=VOP){valid=0;break;}
     Op *op=vv.op;Json *pattern=ji(match,j),*params=jo(pattern,"args");
     if(strcmp(op->name,jg(pattern,"op"))||op->args->n!=jlen(params)){valid=0;break;}
     for(int k=0;k<jlen(params);k++){
      const char *name=js(ji(params,k));Val arg=op->args->a[k];
      if(is_identifier(name)){
       if(map_has(env,name) && !eq_val(map_get(env,name),arg)){valid=0;break;}
       map_put(c,env,name,arg);
      }else if(strcmp(name,val_string(c,arg))!=0){valid=0;break;}
     }
    }
    if(valid && truth(eval(c,jo(rule,"when"),env,caps))){
     Vec *result=new_vec(c);int old_effect=0,new_effect=0;
     for(int j=0;j<count;j++)old_effect+=op_effect(jo(jo(lls_spec(c),"op_by_name"),ops->a[i+j].op->name));
     for(int j=0;j<jlen(replace)&&!c->error[0];j++){
      Json *rep=ji(replace,j),*params=jo(rep,"args");Op *op=(Op*)ca(c,sizeof(Op));op->name=jg(rep,"op");op->args=new_vec(c);
      for(int k=0;k<jlen(params);k++)vec_push(c,op->args,eval(c,ji(params,k),env,caps));
      vec_push(c,result,vop(op));new_effect+=op_effect(jo(jo(lls_spec(c),"op_by_name"),op->name));
     }
     if(old_effect!=new_effect){error(c,"REWRITE: stack height changed in %s",jg(rule,"name"));return;}
     Vec matched=LLS_ZERO;matched.n=count;matched.a=ops->a+i;int same=same_ops(&matched,result);
     if(!same){if(--fuel<0){error(c,"REWRITE: rewrite fuel exhausted");return;}Vec *next=new_vec(c);
      for(int j=0;j<i;j++)vec_push(c,next,ops->a[j]);
      for(int j=0;j<result->n;j++)vec_push(c,next,result->a[j]);
      for(int j=i+count;j<ops->n;j++)vec_push(c,next,ops->a[j]);
      *ops=*next;changed=1;
      i+=result->n>0?result->n:1;
     }else i++;
    }else i++;
   }
  }
  if(!changed||!jnumber(jo(lls_spec(c),"rewrite_repeat")))return;
 }
 if(!c->error[0])error(c,"REWRITE: iteration budget exhausted (possible cycle)");
}

typedef struct Builder {Context *ctx;char *data;size_t n,cap;} Builder;
static void build(Builder *b,const char *fmt,...){char chunk[256];va_list ap;va_start(ap,fmt);int n=vsnprintf(chunk,sizeof(chunk),fmt,ap);va_end(ap);if(n<0)return;size_t len=(size_t)n;
 if(len>=sizeof chunk){char *tmp=(char*)ca(b->ctx,len+1);va_start(ap,fmt);vsnprintf(tmp,len+1,fmt,ap);va_end(ap);if(b->n+len+1>b->cap){b->cap=(b->n+len+256)*2;b->data=(char*)crealloc(b->ctx,b->data,b->cap);}memcpy(b->data+b->n,tmp,len);b->n+=len;b->data[b->n]=0;return;}
 if(b->n+len+1>b->cap){b->cap=(b->n+len+256)*2;b->data=(char*)crealloc(b->ctx,b->data,b->cap);}memcpy(b->data+b->n,chunk,len);b->n+=len;b->data[b->n]=0;
}
static void render_template(Context *c,Builder *b,const char *source,Map *params){
 for(const char *p=source;*p && !c->error[0];){
  if(p[0]=='$' && p[1]=='{'){
   const char *end=strchr(p+2,'}');if(!end){error(c,"BACKEND: unterminated template variable");return;}
   char name[128];size_t len=(size_t)(end-(p+2));if(len>=sizeof name){error(c,"BACKEND: template variable too long");return;}
   memcpy(name,p+2,len);name[len]=0;if(!map_has(params,name)){error(c,"BACKEND: missing template parameter %s",name);return;}
   build(b,"%s",val_string(c,map_get(params,name)));p=end+1;
  }else{build(b,"%c",*p++);}
 }
}
static int bytecode_id(const char *name){
 const char *names[]={"PUSH_F64","ADD","SUB","MUL","DIV","NEG","DUP","SWAP","DROP","PRINT"};
 const char *shortname=strrchr(name,'.');if(shortname)name=shortname+1;
 for(int i=0;i<10;i++)if(!strcmp(name,names[i]))return i;
 return -1;
}
static void encode_bytecode(Context *c,Builder *b,Vec *ops){
 const int pops[]={0,2,2,2,2,1,1,2,1,1},pushes[]={1,1,1,1,1,1,2,2,0,1};
 if(ops->n>1000000){error(c,"BYTECODE: instruction limit exceeded");return;}
 build(b,"LLSBC%c",1);
 uint32_t count=(uint32_t)ops->n;for(int i=0;i<4;i++)build(b,"%c",(int)((count>>(i*8))&255));
 for(int i=0;i<ops->n&&!c->error[0];i++){
  Op *op=ops->a[i].op;int id=bytecode_id(op->name);
  if(id<0){error(c,"BYTECODE: no VM implementation for %s",op->name);return;}
  Json *decl=jo(jo(lls_spec(c),"op_by_name"),op->name);
  if(jnumber(jo(decl,"pop"))!=pops[id]||jnumber(jo(decl,"push"))!=pushes[id]||op->args->n!=(id==0?1:0)){error(c,"BYTECODE: incompatible VM signature");return;}
  Json *in=jo(decl,"inputs"),*out=jo(decl,"outputs");
  for(int j=0;j<jlen(in);j++)if(!jeq(ji(in,j),"Float64")){error(c,"BYTECODE: incompatible VM type");return;}
  for(int j=0;j<jlen(out);j++)if(!jeq(ji(out,j),"Float64")){error(c,"BYTECODE: incompatible VM type");return;}
  build(b,"%c",id);
  if(id==0){double value=number(op->args->a[0]);uint64_t bits;memcpy(&bits,&value,8);for(int j=0;j<8;j++)build(b,"%c",(int)((bits>>(j*8))&255));}
 }
}
static Val execute_bytecode(Context *c,const unsigned char *data,size_t size){
 const int pops[]={0,2,2,2,2,1,1,2,1,1},pushes[]={1,1,1,1,1,1,2,2,0,1};
 Val nil=LLS_ZERO;
 if(size<10||memcmp(data,"LLSBC\1",6)){error(c,"BYTECODE: invalid bytecode header");return nil;}
 uint32_t count=0;for(int i=0;i<4;i++)count|=(uint32_t)data[6+i]<<(i*8);
 if(count>1000000){error(c,"BYTECODE: instruction limit exceeded");return nil;}
 Vec *code=new_vec(c);size_t pos=10;int height=0;
 for(uint32_t i=0;i<count;i++){
  if(pos>=size){error(c,"BYTECODE: truncated instruction");return nil;}
  int id=data[pos++];if(id>=10){error(c,"BYTECODE: unknown instruction");return nil;}
  Val op=LLS_ZERO;op.tag=id;
  if(id==0){if(size-pos<8){error(c,"BYTECODE: truncated Float64 operand");return nil;}uint64_t bits=0;for(int j=0;j<8;j++)bits|=(uint64_t)data[pos++]<<(j*8);memcpy(&op.n,&bits,8);}
  if(height<pops[id]){error(c,"BYTECODE: stack underflow in bytecode");return nil;}
  height+=pushes[id]-pops[id];vec_push(c,code,op);
 }
 if(pos!=size||height!=1){error(c,"BYTECODE: invalid bytecode length or final stack height");return nil;}
 double *stack=(double*)ca(c,((size_t)count+1)*sizeof(double));int sp=0;
 for(int i=0;i<code->n&&!c->error[0];i++){Val op=code->a[i];int id=op.tag;
  if(id==0)stack[sp++]=op.n;
  else if(id==5)stack[sp-1]=-stack[sp-1];
  else if(id==6){stack[sp]=stack[sp-1];sp++;}
  else if(id==7){double a=stack[sp-1];stack[sp-1]=stack[sp-2];stack[sp-2]=a;}
  else if(id==8)sp--;
  else if(id==9)printf("%s\n",val_string(c,vnum(stack[sp-1])));
  else{double b=stack[--sp],a=stack[--sp];if(id==4&&b==0){throw_error(c,"!division_by_zero",NULL);return nil;}stack[sp++]=id==1?a+b:id==2?a-b:id==3?a*b:a/b;}
 }
 return vnum(stack[0]);
}

static void compile_output(Context *c,Builder *b,const char *target,Vec *ops){
 if(verify_ops(c,ops)<0)return;
 Json *backends=jo(lls_spec(c),"backends_compiled");
 for(int i=0;i<jlen(backends);i++){
  Json *backend=ji(backends,i);if(strcmp(jg(backend,"target"),target))continue;
  render_template(c,b,jg(backend,"prologue"),NULL);
  for(int j=0;j<ops->n && !c->error[0];j++){
   Op *op=ops->a[j].op;const char *ir=jg(backend,"ir");size_t irlen=strlen(ir);
   if(strncmp(op->name,ir,irlen)||op->name[irlen]!='.'){error(c,"BACKEND: operation belongs to another IR");return;}
   const char *shortname=strrchr(op->name,'.');shortname=shortname?shortname+1:op->name;
   Json *mapping=jo(jo(backend,"emits"),shortname);
   if(!mapping){error(c,"BACKEND: missing %s emission for %s",target,op->name);return;}
   Json *params=jo(mapping,"params");
   if(jlen(params)!=op->args->n){error(c,"BACKEND: wrong operand count for %s",op->name);return;}
   Map *args=new_map(c);
   for(int k=0;k<jlen(params);k++){Val value=op->args->a[k];if(value.tag==VSTR)value=vstr(quoted_string(c,value.s,!strcmp(target,"python3")));else if(value.tag==VNUM)value=vstr(number_literal(c,value.n,!strcmp(target,"python3")));map_put(c,args,js(ji(params,k)),value);}
   render_template(c,b,jg(mapping,"template"),args);
  }
  render_template(c,b,jg(backend,"epilogue"),NULL);return;
 }
 if(strcmp(target,"bytecode")==0){encode_bytecode(c,b,ops);return;}
 int c_output=!strcmp(target,"c99")||!strcmp(target,"c11")||!strcmp(target,"cpp17")||!strcmp(target,"cpp20"),py_output=strcmp(target,"python3")==0;
 if(!c_output&&!py_output){error(c,"BACKEND: unsupported target %s",target);return;}
 const int expected_pop[]={0,2,2,2,2,1,1,2,1,1},expected_push[]={1,1,1,1,1,1,2,2,0,1};
 for(int i=0;i<ops->n;i++){
  Op *op=ops->a[i].op;int id=bytecode_id(op->name);Json *decl=jo(jo(lls_spec(c),"op_by_name"),op->name);
  if(id<0||jnumber(jo(decl,"pop"))!=expected_pop[id]||jnumber(jo(decl,"push"))!=expected_push[id]||op->args->n!=(id==0?1:0)){error(c,"BACKEND: incompatible built-in signature for %s",op->name);return;}
  Json *in=jo(decl,"inputs"),*out=jo(decl,"outputs");
  for(int j=0;j<jlen(in);j++)if(!jeq(ji(in,j),"Float64")){error(c,"BACKEND: incompatible built-in stack type");return;}
  for(int j=0;j<jlen(out);j++)if(!jeq(ji(out,j),"Float64")){error(c,"BACKEND: incompatible built-in stack type");return;}
 }
 if(c_output)build(b,"#include <stdio.h>\n#include <stdlib.h>\n#include <math.h>\nint main(void){\n double s[%d],a=0,b=0;int sp=0;(void)a;(void)b;\n",ops->n+8);
 else build(b,"#!/usr/bin/env python3\nimport sys\ns=[]\n");
 for(int i=0;i<ops->n;i++){
  Op *o=ops->a[i].op;const char *n=strrchr(o->name,'.');n=n?n+1:o->name;
  if(strcmp(n,"PUSH_F64")==0){double v=o->args->n?number(o->args->a[0]):0;if(c_output)build(b," s[sp++]=%s;\n",number_literal(c,v,0));else build(b,"s.append(%s)\n",number_literal(c,v,1));}
  else if(strcmp(n,"DUP")==0){build(b,c_output?" s[sp]=s[sp-1];sp++;\n":"s.append(s[-1])\n");}
  else if(strcmp(n,"SWAP")==0){build(b,c_output?" a=s[sp-1];s[sp-1]=s[sp-2];s[sp-2]=a;\n":"s[-1],s[-2]=s[-2],s[-1]\n");}
  else if(strcmp(n,"DROP")==0){build(b,c_output?" sp--;\n":"s.pop()\n");}
  else if(strcmp(n,"PRINT")==0){build(b,c_output?" printf(\"%%.15g\\n\",s[sp-1]);\n":"print(format(s[-1],'.15g'))\n");}
  else if(strcmp(n,"NEG")==0){build(b,c_output?" s[sp-1]=-s[sp-1];\n":"s.append(-s.pop())\n");}
  else if(!strcmp(n,"ADD")||!strcmp(n,"SUB")||!strcmp(n,"MUL")||!strcmp(n,"DIV")){
   char op=!strcmp(n,"ADD")?'+':!strcmp(n,"SUB")?'-':!strcmp(n,"MUL")?'*':'/';
   build(b,c_output?" b=s[--sp];a=s[--sp];\n":"b=s.pop();a=s.pop()\n");
   if(op=='/')build(b,c_output?" if(b==0){fputs(\"!division_by_zero\\n\",stderr);return 1;}\n":"if b == 0: sys.exit('!division_by_zero')\n");
   if(c_output)build(b," s[sp++]=a%c b;\n",op);else build(b,"s.append(a %c b)\n",op);
  }
  else {error(c,"BACKEND: unsupported IR operation %s",o->name);return;}
 }
 if(c_output)build(b," if(sp!=1)return 1;\n printf(\"%%.15g\\n\",s[0]);return 0;\n}\n");
 else build(b,"if len(s)!=1: sys.exit('!bad_result_count')\nprint(format(s[-1],'.15g'))\n");
}
static char *read_source(Context *c,const char *path,int binary,size_t *size){FILE *f=strcmp(path,"-")==0?stdin:fopen(path,"rb");if(!f)return NULL;c->input_file=f;Builder b=LLS_ZERO;b.ctx=c;char buf[4096];size_t n;
 while((n=fread(buf,1,sizeof(buf),f))>0){if(!binary&&memchr(buf,0,n)){c->input_file=NULL;if(f!=stdin)fclose(f);error(c,"LEX: embedded NUL is not supported");return NULL;}if(b.n+n+1>b.cap){b.cap=(b.n+n+4096)*2;b.data=(char*)crealloc(c,b.data,b.cap);}memcpy(b.data+b.n,buf,n);b.n+=n;b.data[b.n]=0;}
 c->input_file=NULL;if(f!=stdin)fclose(f);
 if(!b.data)b.data=dup_n(c,"",0);
 *size=b.n;return b.data;
}
#ifdef __cplusplus
extern "C"
#endif
int llsynth_generated_main(int argc,char **argv){const char *file="-",*output=NULL,*target=LLS_TARGET;int action=!strcmp(LLS_MODE,"compile")||!strcmp(LLS_MODE,"rewrite")?1:!strcmp(LLS_MODE,"analyze")?5:0;
 for(int i=1;i<argc;i++){
  if(strcmp(argv[i],"--interpret")==0)action=0;
  else if(strcmp(argv[i],"--compile")==0||strcmp(argv[i],"--rewrite")==0)action=1;
  else if(strcmp(argv[i],"--analyze")==0)action=5;
  else if(strcmp(argv[i],"--run-bytecode")==0)action=4;
  else if(strcmp(argv[i],"--tokens")==0)action=2;
  else if(strcmp(argv[i],"--parse")==0)action=3;
  else if(strcmp(argv[i],"--target")==0&&i+1<argc)target=argv[++i];
  else if((strcmp(argv[i],"-o")==0||strcmp(argv[i],"--output")==0)&&i+1<argc)output=argv[++i];
  else if(strcmp(argv[i],"--help")==0){puts("--interpret | --compile | --analyze | --rewrite | --run-bytecode | --tokens | --parse [--target c99|python3|bytecode] [-o output] [file|-]");return 0;}
  else if(argv[i][0]!='-'||strcmp(argv[i],"-")==0)file=argv[i];
  else {fprintf(stderr,"unknown argument: %s\n",argv[i]);return 2;}
 }
 if(action==0&&strcmp(LLS_MODE,"both")!=0&&strcmp(LLS_MODE,"interpret")!=0){fputs("interpreter not generated\n",stderr);return 2;}
 if(action==1&&strcmp(LLS_MODE,"both")!=0&&strcmp(LLS_MODE,"compile")!=0&&strcmp(LLS_MODE,"rewrite")!=0){fputs("compiler not generated\n",stderr);return 2;}
 Context *ctx=(Context*)calloc(1,sizeof(Context));if(!ctx){fputs("out of memory\n",stderr);return 2;}
 jmp_buf handler;ctx->oom=&handler;
 if(setjmp(handler)){fputs("out of memory\n",stderr);clear_ctx(ctx);free(ctx);return 2;}
 reset(ctx);
 size_t source_size=0;char *source=read_source(ctx,file,action==4,&source_size);if(!source){fprintf(stderr,"unable to read %s: %s\n",file,ctx->error);clear_ctx(ctx);free(ctx);return 2;}
 Builder result=LLS_ZERO;result.ctx=ctx;
 if(action==5){(void)run(ctx,source,"analyze");if(!ctx->error[0])build(&result,"analysis OK\n");}
 else if(action==4){Val v=execute_bytecode(ctx,(const unsigned char*)source,source_size);if(!ctx->error[0])build(&result,"%s\n",val_string(ctx,v));}
 else if(action==2){Vec *v=lex(ctx,source);for(int i=0;i<v->n;i++){Token *t=(Token*)v->a[i].s;build(&result,"%d:%d %s [%s]\n",t->line,t->col,t->name,t->text);}}
 else if(action==3){Node *node=parse(ctx,source);if(node)build(&result,"parse OK: %s\n",node->name);}
 else if(action==0){Val v=run(ctx,source,"semantics");if(!ctx->error[0])build(&result,"%s\n",val_string(ctx,v));}
 else {Val v=run(ctx,source,"lower");if(!ctx->error[0]){
   Vec *code=v.tag==VVEC?v.vec:map_get(ctx->states,"code").vec;
   if(code){rewrite_ops(ctx,code);if(!ctx->error[0])compile_output(ctx,&result,target,code);}
   else error(ctx,"BACKEND: missing IR code vector");
  }}
 int status=0;
 if(ctx->error[0]){if(ctx->line)fprintf(stderr,"%s: %d:%d: %s\n",jg(lls_spec(ctx),"language"),ctx->line,ctx->col,ctx->error);
  else fprintf(stderr,"%s: %s\n",jg(lls_spec(ctx),"language"),ctx->error);
  if(ctx->rule_stack[0])fprintf(stderr,"rules: %s\n",ctx->rule_stack);
  status=1;}
 else {FILE *f=output?fopen(output,"wb"):stdout;if(!f){fprintf(stderr,"unable to write %s\n",output);status=2;}
  else {fwrite(result.data?result.data:"",1,result.n,f);if(output)fclose(f);}}
 clear_ctx(ctx);free(ctx);return status;
}
