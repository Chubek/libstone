#!/usr/bin/env perl
use strict;
use warnings;
use utf8;
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
my %opt=(T=>'python3', mode=>'both', split=>'explicit', diagnostics=>'text', parser=>'', program_target=>'c99', out_dir=>'.', driver_mode=>'cli');
my @include_paths;
GetOptions('T|tool-target=s'=>\$opt{T},'o|output=s'=>\$opt{output},'h|header=s'=>\$opt{header},
  'mode=s'=>\$opt{mode},'program-target=s'=>\$opt{program_target},'parser=s'=>\$opt{parser},
  'split=s'=>\$opt{split},'out-dir=s'=>\$opt{out_dir},'package=s'=>\$opt{package},
  'runtime=s'=>\$opt{runtime},'semant=s'=>\$opt{semant},'rewriter=s'=>\$opt{rewriter},
  'table=s'=>\$opt{table},'lexer=s'=>\$opt{lexer},'parser-out=s'=>\$opt{parser_out},
  'ast=s'=>\$opt{ast},'ir=s'=>\$opt{ir},'driver=s'=>\$opt{driver},'manifest=s'=>\$opt{manifest},
  'check'=>\$opt{check},'help'=>\$opt{help},'version'=>\$opt{version},'diagnostics=s'=>\$opt{diagnostics},
  'dump-grammar:s'=>\$opt{dump_grammar},'dump-tables:s'=>\$opt{dump_tables},
  'warnings-as-errors'=>\$opt{warnings_as_errors}, 'include-path=s@'=>\@include_paths,
  'module-prefix=s'=>\$opt{module_prefix},'driver-mode=s'=>\$opt{driver_mode},'deterministic'=>\$opt{deterministic},
  'trace-parser'=>\$opt{trace_parser},'max-errors=i'=>\$opt{max_errors},
  'depfile=s'=>\$opt{depfile},'emit-table-format=s'=>\$opt{table_format}) or usage(2);
if($opt{help}){usage(0)}
if($opt{version}){print "llsynth.pl $VERSION\n"; exit 0}
@ARGV==1 or usage(2);
my $input=$ARGV[0];
my $json=JSON::PP->new->utf8->canonical->allow_nonref;
my $spec;
eval {
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
if($opt{T} eq 'python3') { @outputs=generate_python($spec,\%opt,$json) }
else { @outputs=generate_c($spec,\%opt,$json) }
if($opt{manifest}) {
  my $manifest={generator=>'llsynth.pl',version=>$VERSION,language=>$spec->{language},
    dependencies=>$spec->{dependencies},source=>$input, source_sha256=>sha256_hex(slurp($input)), options=>{map { $_=>$opt{$_} } grep {defined $opt{$_} && !ref($opt{$_})} sort keys %opt},
    outputs=>[map { {path=>$_,sha256=>sha256_hex(slurp($_))} } @outputs]};
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
  --dump-tables[=FILE]       show generated parser tables
  --dump-grammar[=FILE]      show expanded grammar
  --out-dir DIR              directory for emitted files
  --driver-mode MODE         cli, library, both
  --package NAME             generate a Python package
  --module-prefix NAME       C/C++ symbol prefix
  --trace-parser             trace generated parser actions
  --include-path DIR         additional textual include root
  --depfile FILE             Make/Ninja dependencies
  --manifest FILE            output SHA256 manifest
  --diagnostics text|json|sarif generator error format
  --help, --version          information
TXT
 print $h;exit $status;
}
sub fail {my($s,$f,$line)=@_; die "".($f||'<input>').':'.($line||1).": error: $s\n"}
sub slurp {my($f)=@_;open my $fh,'<:raw',$f or die "$f: $!\n";local $/;my $s=<$fh>;close $fh;return $s}
sub write_file {
 my($f,$s)=@_;
 my %inputs=map {canonical_path($_)=>1} ($input,__FILE__,map {$_->{path}} @{$spec->{dependencies}||[]});
 !$inputs{canonical_path($f)} or die "output would overwrite an input: $f\n";
 -d dirname($f) or die "output directory does not exist: ".dirname($f)."\n";
 my($fh,$tmp)=tempfile('.llsynth-XXXXXXXX',DIR=>dirname($f),UNLINK=>0);
 binmode $fh;
 my $ok=eval {print $fh $s or die "$tmp: $!\n";close $fh or die "$tmp: $!\n";rename $tmp,$f or die "$tmp -> $f: $!\n";1};
 if(!$ok){my $error=$@;close $fh;unlink $tmp;die $error}
}

sub outpath {my($f)=@_;return $f if $f =~ m{^(?:/|\./|\.\./)};return "$opt{out_dir}/$f"}
sub strip_ws {my($s)=@_;$s//= ''; $s =~ s/^\s+|\s+$//g;return $s}
sub read_spec_lines {
 my($file,$active,$deps)=@_;my $path=canonical_path($file);
 $active->{$path} and fail("include cycle involving $file",$file,1);
 $active->{$path}=1;
 my $bytes=slurp($file);$deps->{$path}=sha256_hex($bytes);
 my $src=eval {Encode::decode('UTF-8',$bytes,Encode::FB_CROAK())};
 $@ and fail('invalid UTF-8 in specification',$file,1);
 index($src,chr(0))<0 or fail('NUL bytes are not supported in specifications',$file,1);
 $src=~s/\r\n?/\n/g;
 my @out;my @lines=split /\n/,$src,-1;
 for my $i(0..$#lines){my $line=$lines[$i];
  if($line =~ /^\s*\.include\s+"([^"\n]+)"\s*$/){my $name=$1;my @roots=(dirname($file),@include_paths);my $found;
   for my $root(@roots){my $candidate=File::Spec->file_name_is_absolute($name)?$name:File::Spec->catfile($root,$name);if(-f $candidate){$found=$candidate;last}}
   defined $found or fail("include '$name' not found",$file,$i+1);
   push @out,read_spec_lines($found,$active,$deps);
  }else{push @out,[$line,$file,$i+1]}
 }
 delete $active->{$path};return @out;
}

sub parse_spec {
 my($file)=@_;my %deps;my @lines=read_spec_lines($file,{},\%deps);
 my $s={dependencies=>[map {{path=>$_,sha256=>$deps{$_}}} sort keys %deps],file=>$file,language=>'',tokens=>[],skips=>[],rules=>[],functions=>[],states=>[],tables=>[],exceptions=>[],irs=>[],options=>{},init=>'',rewrites=>[],backends=>[],analyses=>[],pipelines=>[],precedence=>[],internal=>{}};
 my $block='';my $cur;my $facet='';my $body='';
 for(my $n=0;$n<@lines;$n++){
  my($raw,$origin,$ln)=@{$lines[$n]};$file=$origin;my $l=strip_ws($raw);
  next if $l eq '' && !$block;
  next if $l =~ /^#(?:\s|$)/;
  if($block eq 'rule') {
   if($l eq '..') {push @{$s->{rules}},$cur; $block='';$facet='';next}
   if($l =~ /^'(syntax|semantics|lower|analyze|emit)\s*:\s*(.*)$/) {$facet=$1;$cur->{facets}{$facet} .= "$2\n";next}
   if($l eq "'end"){$facet='';next}
   $facet or fail("rule content without facet",$file,$ln);
   $cur->{facets}{$facet}.="$raw\n";
   next;
  }
  if($block) {
   if($l eq '.end') {
    if($block eq 'function'){$cur->{body}=$body;push @{$s->{functions}},$cur}
    elsif($block eq 'init'){$s->{init}.=$body}
    elsif($block eq 'ir'){push @{$s->{irs}},$cur}
    elsif($block eq 'rewrite'){push @{$s->{rewrites}},$cur}
    elsif($block eq 'backend'){push @{$s->{backends}},$cur}
    elsif($block eq 'analysis'){push @{$s->{analyses}},$cur}
    elsif($block eq 'pipeline'){push @{$s->{pipelines}},$cur}
    $block='';$body='';$cur=undef;next;
   }
   if($block eq 'ir') {
    if($l =~ /^\.op\s+(\w+)(?:\((.*?)\))?\s*:\s*(.*)$/){
      my ($name,$params,$spec)=($1,$2,$3);my($pop,$push)=(0,0);my(@inputs,@outputs);
      $spec=~/\([^)]*\)\s*->\s*\([^)]*\)/ or fail('this release requires explicit stack signatures for IR operations',$file,$ln);
      if($spec =~ /\(([^)]*)\)\s*->\s*\(([^)]*)\)/){@inputs=grep {length} map {strip_ws($_)} split /,/,$1;@outputs=grep {length} map {strip_ws($_)} split /,/,$2;$pop=@inputs;$push=@outputs}
      push @{$cur->{ops}}, {name=>$name,params=>$params||'',pop=>$pop,push=>$push,inputs=>\@inputs,outputs=>\@outputs,effects=>($spec=~/effect\(([^)]*)\)/?[map {strip_ws($_)} split /,/,$1]:[])};next;
    }
    fail("invalid IR operation",$file,$ln);
   }
   if($block =~ /^(?:rewrite|backend|analysis|pipeline)$/){$cur->{body}.="$raw\n";next}
   $body.="$raw\n";next;
  }
  if($l =~ /^\.language\s+"([^"]+)"/){$s->{language}=$1;next}
  if($l =~ /^\.version\s+"([^"]+)"/){$s->{version}=$1;next}
  if($l =~ /^\.summary\s+"([^"]+)"/){$s->{summary}=$1;next}
  if($l =~ /^\.start\s+(?:"([^"]+)"|(\w+))/){$s->{start}=$1||$2;next}
  if($l =~ /^\.precedence\s+(left|right|nonassoc)\s+(.+)$/){my($assoc,$rest)=($1,$2);my @terms=($rest =~ /\"[^\"]+\"|\S+/g);push @{$s->{precedence}},{assoc=>$assoc,terms=>\@terms};next}
  if($l =~ /^\.option\s+(\w+)\s*=\s*(.*)$/){my($k,$v)=($1,strip_ws($2));$v=~s/^"|"$//g;$s->{options}{$k}=$v;next}
  if($l =~ /^\.skip\s+\/(.*)\/\s*$/){push @{$s->{skips}}, {regex=>$1,line=>$ln};next}
  if($l =~ /^\.token\s+(\w+)(?:\s*:\s*([\w.]+))?\s*=\s*\/(.*)\/\s*$/){push @{$s->{tokens}}, {name=>$1,type=>$2||'String',regex=>$3,line=>$ln};next}
  if($l =~ /^\.literal\s+(\w+)\s*=\s*"(.*)"$/){push @{$s->{tokens}},{name=>$1,type=>'String',literal=>decode_string($2),regex=>quotemeta(decode_string($2)),line=>$ln};next}
  if($l =~ /^\.type\s+%(\w+)\s*(?:=\s*(.*))?$/){push @{$s->{types}}, {name=>$1,type=>$2||'Any'};next}
  if($l =~ /^\.state\s+\@(\w+)\s*:\s*(.*?)\s*=\s*(.*)$/){push @{$s->{states}},{name=>$1,type=>$2,initial=>strip_ws($3)};next}
  if($l =~ /^\.symtbl\s+\#(\w+)(?:\s*:\s*(.*))?$/){push @{$s->{tables}},{name=>$1,type=>$2||'Any'};next}
  if($l =~ /^\.exception\s+\!(\w+)(?:\((.*?)\))?\s+"(.*)"$/){push @{$s->{exceptions}},{name=>$1,params=>$2||'',message=>decode_string($3)};next}
  if($l =~ /^\.rule\s+(\w+)$/){$block='rule';$cur={name=>$1,facets=>{},line=>$ln};next}
  if($l =~ /^\.function\s+(\w+)\s*\((.*?)\)\s*->\s*(.*)$/){$block='function';$cur={name=>$1,args=>$2,returns=>$3,line=>$ln};$body='';next}
  if($l eq '.init'){$block='init';$body='';next}
  if($l =~ /^\.ir\s+(\w+)/){$block='ir';$cur={name=>$1,ops=>[]};next}
  if($l =~ /^\.(rewrite|analysis|backend|pipeline)\s+(.+)$/){$block=$1;$cur={name=>$2,body=>''};next}
  fail("unknown directive or unexpected text: '$l'",$file,$ln);
 }
 $block and fail("unterminated '$block' block",$file,scalar @lines);
 $s->{language} ||= 'Language'; $s->{start} ||= ($s->{rules}[0]||{})->{name};
 return $s;
}
sub validate_declarations {
 my($s)=@_;
 for my $kind(qw(tokens rules functions states tables exceptions types irs)){
  my %seen;
  for my $d(@{$s->{$kind}||[]}){
   $seen{$d->{name}}++ and fail("duplicate $kind declaration '$d->{name}'",$s->{file},$d->{line}||1);
   $d->{name}=~/^(?:__lls_|__LLS_|EOF$|LIT_)/ and fail("reserved name '$d->{name}'",$s->{file},$d->{line}||1);
  }
 }
 my %tokens=map {$_->{name}=>1} @{$s->{tokens}};
 for my $r(@{$s->{rules}}){$tokens{$r->{name}} and fail("rule/token name collision '$r->{name}'",$s->{file},$r->{line})}
 for my $ir(@{$s->{irs}}){my %seen;for my $op(@{$ir->{ops}}){$seen{$op->{name}}++ and fail("duplicate IR operation $ir->{name}.$op->{name}",$s->{file},1)}}
 my($start)=grep {$_->{name} eq ($s->{start}||'')} @{$s->{rules}};
 if($start){
  for my $facet($opt{mode} eq 'both'?qw(semantics lower):$opt{mode} eq 'interpret'?'semantics':$opt{mode} =~ /^(compile|rewrite)$/?'lower':()){
   defined $start->{facets}{$facet} or fail("start rule requires '$facet facet for mode $opt{mode}",$s->{file},$start->{line});
  }
 }
}
sub validate_productions {
 my($s,$prods,$nonterms,$tokens)=@_;
 my %productive=%$tokens;my %last=map {$_=>{$_=>1}} keys %$tokens;my %nullable;my $change=1;
 while($change){$change=0;
  for my $p(@$prods){
   if(!$productive{$p->{lhs}} && !grep {!$productive{$_}} @{$p->{rhs}}){$productive{$p->{lhs}}=1;$change=1}
   if(!$nullable{$p->{lhs}} && !grep {!$nullable{$_}} @{$p->{rhs}}){$nullable{$p->{lhs}}=1;$change=1}
   for my $sym(reverse @{$p->{rhs}}){for my $t(keys %{$last{$sym}||{}}){unless($last{$p->{lhs}}{$t}){$last{$p->{lhs}}{$t}=1;$change=1}}last unless $nullable{$sym}}
  }
 }
 for my $r(@{$s->{rules}}){$productive{$r->{name}} or fail("unproductive grammar rule $r->{name}",$s->{file},$r->{line})}
 ($last{$s->{start}}{EOF} && keys(%{$last{$s->{start}}})==1 && !$nullable{$s->{start}})
  or fail('every start-rule derivation must end in EOF',$s->{file},1);
 for my $p(@$prods){my %labels;for my $label(grep {length} @{$p->{labels}}){$labels{$label}++ and fail("duplicate capture '$label' in $p->{lhs}; use a labeled repetition",$s->{file},1)}}
 my %reachable=($s->{start}=>1);$change=1;
 while($change){$change=0;for my $p(@$prods){next unless $reachable{$p->{lhs}};for my $sym(@{$p->{rhs}}){unless($reachable{$sym}){$reachable{$sym}=1;$change=1}}}}
 for my $r(@{$s->{rules}}){next if $reachable{$r->{name}};my $msg="unreachable rule $r->{name}";
  fail($msg,$s->{file},$r->{line}) if $opt{warnings_as_errors};
  print STDERR "$s->{file}:$r->{line}: warning W001: $msg\n" if $opt{diagnostics} eq 'text';
 }
}

# Grammar tokens: syntax is deliberately separated from action-language tokens.
sub decode_string {
 my($text)=@_;my $out='';my %esc=(n=>"\n",r=>"\r",t=>"\t",b=>"\b",f=>"\f",'\\'=>'\\','"'=>'"',"'"=>"'",'/'=>'/');
 while(length $text){
  if($text=~s/^([^\\]+)//s){$out.=$1;next}
  $text=~s/^\\(.)//s or die "unterminated string escape\n";my $escape=$1;
  if(exists $esc{$escape}){$out.=$esc{$escape};next}
  if($escape eq 'u'||$escape eq 'x'){
   my $width=$escape eq 'u'?4:2;
   $text=~s/^([0-9A-Fa-f]{$width})// or die "invalid hexadecimal string escape\n";my $value=hex($1);
   if($value>=0xd800 && $value<=0xdbff){
    $text=~s/^\\u([0-9A-Fa-f]{4})// or die "unpaired Unicode surrogate in string\n";my $low=hex($1);
    $low>=0xdc00 && $low<=0xdfff or die "invalid Unicode surrogate pair\n";
    $value=0x10000+(($value-0xd800)<<10)+$low-0xdc00;
   }elsif($value>=0xdc00 && $value<=0xdfff){die "unpaired Unicode surrogate in string\n"}
   $out.=chr($value);next;
  }
  die "unsupported string escape \\$escape\n";
 }
 index($out,chr(0))<0 or die "NUL bytes are not supported in strings\n";
 return $out;
}

sub gram_tokens {my($s)=@_;my @out;while($s =~ /\G\s*(?:(\w+)|("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')|([\(\)\|\*\+\?\:\%]))/gc){push @out,defined($1)?{k=>'id',v=>$1}:defined($2)?{k=>'lit',v=>decode_string(substr($2,1,-1))}:{k=>$3,v=>$3}}
 my $rest=substr($s,pos($s)//0);strip_ws($rest) eq '' or die "invalid grammar fragment '$rest'\n";return \@out;}
sub gpeek {my($st)=@_;return $st->{t}[$st->{i}]||{k=>'eof',v=>''}}
sub gtake {my($st)=@_;return $st->{t}[$st->{i}++]}
sub g_alt {my($st)=@_;my @v=(g_seq($st));while(gpeek($st)->{k} eq '|'){gtake($st);push @v,g_seq($st)}return @v==1?$v[0]:{k=>'alt',v=>\@v}}
sub g_seq {my($st)=@_;my @v;while(gpeek($st)->{k} =~ /^(?:id|lit|\()$/){push @v,g_post($st)}return @v==1?$v[0]:{k=>'seq',v=>\@v}}
sub g_post {my($st)=@_;my $t=gtake($st);my $r;
 if($t->{k} eq '('){$r=g_alt($st);gtake($st)->{k} eq ')' or die "expected ')' in grammar\n"}
 elsif($t->{k} eq 'id' && gpeek($st)->{k} eq ':'){gtake($st);my $x=g_post($st);$r={k=>'label',name=>$t->{v},v=>$x}}
 else {$r={k=>'sym',name=>$t->{v},literal=>$t->{k} eq 'lit'?1:0}}
 if(gpeek($st)->{k} =~ /^(?:\*|\+|\?)$/){$r={k=>gtake($st)->{k},v=>$r}}
 if(gpeek($st)->{k} eq '%'){gtake($st);$r={k=>'sep',v=>$r,sep=>g_post($st)}}
 return $r;
}
sub compile_grammar {
 my($s,$algorithm)=@_; my @prods;my %nonterms=map {$_->{name}=>1} @{$s->{rules}};
 my %literal_names=map {('__lit__'.$_->{literal})=>$_->{name}} grep {defined $_->{literal}} @{$s->{tokens}}; my $num=0;
 for my $r (@{$s->{rules}}){
   my $synt=$r->{facets}{syntax}//'';strip_ws($synt) ne '' or fail("rule $r->{name} lacks syntax",$s->{file},$r->{line});
   my $tok=gram_tokens($synt);my $st={t=>$tok,i=>0};my $ast=g_alt($st);
   gpeek($st)->{k} eq 'eof' or fail("unexpected symbol in grammar",$s->{file},$r->{line});
   $r->{grammar_ast}=$ast;
 }
 my $gensym=sub {return '__lls_'.++$num};
 my $add;my $symbol;
 $add=sub {my($lhs,$rhs)=@_;push @prods,{lhs=>$lhs,rhs=>[map {$_->{name}} @$rhs],labels=>[map {$_->{label}||''} @$rhs],id=>scalar @prods};};
 $symbol=sub {my($n)=@_;my $k=$n->{k};
  if($k eq 'sym'){
    my $name=$n->{name};if($n->{literal}){my $key='__lit__'.$name;if(!$literal_names{$key}){
       my $id='LIT_'.(scalar keys %literal_names);$literal_names{$key}=$id;
       push @{$s->{tokens}},{name=>$id,regex=>quotemeta($name),type=>'String',line=>0};
      }$name=$literal_names{$key};}
    return {name=>$name};
  }
  if($k eq 'label') {my $x=$symbol->($n->{v});$x->{label}=$n->{name};return $x}
  my $lhs=$gensym->();$nonterms{$lhs}=1;
  $s->{internal}{$lhs}=$k;
  if($k eq 'alt') {for my $a (@{$n->{v}}){my $rhs=$a->{k} eq 'seq'?$a->{v}:[$a];$add->($lhs,[map {$symbol->($_)} @$rhs])}}
  elsif($k eq 'seq'){$add->($lhs,[map {$symbol->($_)} @{$n->{v}}])}
  elsif($k eq '?'){$add->($lhs,[]);$add->($lhs,[$symbol->($n->{v})])}
  elsif($k eq '*'){$add->($lhs,[]);$add->($lhs,[{name=>$lhs},$symbol->($n->{v})])}
  elsif($k eq '+'){$add->($lhs,[$symbol->($n->{v})]);$add->($lhs,[{name=>$lhs},$symbol->($n->{v})])}
  elsif($k eq 'sep'){$add->($lhs,[$symbol->($n->{v})]);$add->($lhs,[{name=>$lhs},$symbol->($n->{sep}),$symbol->($n->{v})])}
  else {die "unsupported grammar construct $k\n"}
  return {name=>$lhs};
 };
 for my $r (@{$s->{rules}}) {
  my $a=$r->{grammar_ast};my @alts=$a->{k} eq 'alt'?@{$a->{v}}:($a);
  for my $alt (@alts){my $rhs=$alt->{k} eq 'seq'?$alt->{v}:[$alt];$add->($r->{name},[map {$symbol->($_)} @$rhs]);}
 }
 $s->{start} && $nonterms{$s->{start}} or fail("start rule does not exist",$s->{file},1);
 my %tokens=map {$_->{name}=>1} @{$s->{tokens}};
 $tokens{EOF}=1;
 my %precedence;my $level=0;
 for my $d(@{$s->{precedence}}){$level++;for my $term(@{$d->{terms}}){
   my $name=$term;
   if($term =~ /^"(.*)"$/){$name=$literal_names{'__lit__'.$1}||$1}
   $precedence{$name}=[$level,$d->{assoc}];
 }}
 for my $p (@prods){for my $sym (@{$p->{rhs}}){$nonterms{$sym} || $tokens{$sym} or fail("undefined grammar symbol '$sym'",$s->{file},1)}}
 validate_productions($s,\@prods,\%nonterms,\%tokens);
 # Append augmented start, then FIRST + LR(1) closure.
 my $aug='__LLS_START';$nonterms{$aug}=1;$add->($aug,[{name=>$s->{start}}]);
 my @by_lhs;my %by; for my $p (@prods){push @{$by{$p->{lhs}}},$p->{id}}
 my %first=map {$_=>{$_=>1}} keys %tokens; $first{'$'}={'$'=>1};
 $first{$_}//={} for keys %nonterms;
 my $updated=1;while($updated){$updated=0;for my $p(@prods){my $lhs=$first{$p->{lhs}}; my $nullable=1;
   for my $sym(@{$p->{rhs}}){ for my $tok(keys %{$first{$sym}}){next if $tok eq '';if(!$lhs->{$tok}){$lhs->{$tok}=1;$updated=1}}
     if(!$first{$sym}{''}){$nullable=0;last}
   } if($nullable && !$lhs->{''}){$lhs->{''}=1;$updated=1}
 }}
 my $fst_seq=sub {my(@seq)=@_;my %out;my $null=1;for my $sym (@seq){for my $k(keys %{$first{$sym}||{}}){$out{$k}=1 unless $k eq ''}if(!$first{$sym}{''}){$null=0;last}}$out{''}=1 if $null;return keys %out};
 my $key=sub {my($it)=@_;return join(';',sort keys %$it)};
 my $close=sub {
  my($it)=@_;my $change=1;
  while($change){$change=0; for my $ik (sort keys %$it){my($pid,$dot,$look)=split /,/, $ik;my $p=$prods[$pid];my $sym=$p->{rhs}[$dot];next unless defined $sym && $nonterms{$sym};
      my @tail=(@{$p->{rhs}}[$dot+1..$#{$p->{rhs}}],$look);
      my @looks=$fst_seq->(@tail);
      for my $pp (@{$by{$sym}||[]}) {for my $l (@looks){next if $l eq '';my $new=join(',',$pp,0,$l);if(!$it->{$new}){$it->{$new}=1;$change=1}}}
  }}return $it};
 my @states;my @trans;my %ids;
 my $initial=$close->({(scalar(@prods)-1).',0,$'=>1});
 push @states,$initial;$ids{$key->($initial)}=0;
 for(my $i=0;$i<@states;$i++){
  my %edges;for my $ik(keys %{$states[$i]}){my($pid,$dot,$la)=split /,/,$ik;my $sym=$prods[$pid]{rhs}[$dot];next unless defined $sym;
    $edges{$sym}{join(',',$pid,$dot+1,$la)}=1;
  }
  for my $sym (sort keys %edges){my $set=$close->($edges{$sym});my $k=$key->($set);my $j;
   if(defined $ids{$k}){$j=$ids{$k}}else{$j=@states;$ids{$k}=$j;push @states,$set}
   $trans[$i]{$sym}=$j;
  }
  @states>3000 and fail('grammar produced >3000 states',$s->{file},1);
 }
 if($algorithm eq 'lalr1') {
  my %core_to_id;my @merged;my @old2new;
  for my $i (0..$#states){my %core=map {my($p,$d)=split/,/; ("$p,$d"=>1)} keys %{$states[$i]};my $core=join(';',sort keys %core);
   if(!exists $core_to_id{$core}){$core_to_id{$core}=@merged;push @merged,{}}
   my $id=$core_to_id{$core};$old2new[$i]=$id;
   $merged[$id]{$_}=1 for keys %{$states[$i]};
  }
  my @mt;for my $i(0..$#trans){for my $sym(keys %{$trans[$i]||{}}){$mt[$old2new[$i]]{$sym}=$old2new[$trans[$i]{$sym}]}}
  @states=@merged;@trans=@mt;
 }
 my (@action,@goto);
 my %prod_prec;
 for my $p(@prods){for my $sym(reverse @{$p->{rhs}}){if($precedence{$sym}){$prod_prec{$p->{id}}=$precedence{$sym};last}}}
 my $resolve=sub{my($term,$old,$new)=@_;
  return undef unless ($old->[0] eq 's' && $new->[0] eq 'r') || ($old->[0] eq 'r' && $new->[0] eq 's');
  my $reduce=$old->[0] eq 'r'?$old:$new;my $shift=$old->[0] eq 's'?$old:$new;
  my ($pa,$pb)=($prod_prec{$reduce->[1]},$precedence{$term});return undef unless $pa && $pb;
  return $pa->[0]>$pb->[0]?$reduce:$pa->[0]<$pb->[0]?$shift:$pa->[1] eq 'left'?$reduce:$pa->[1] eq 'right'?$shift:['x',0];
 };
 for my $i(0..$#states){my %a;for my $ik(sort keys %{$states[$i]}){my($pid,$dot,$la)=split/,/,$ik;my $p=$prods[$pid];my $sym=$p->{rhs}[$dot];my ($key,$val);
    if(defined $sym){if(!$nonterms{$sym}){$key=$sym;$val=['s',$trans[$i]{$sym}]}}
    elsif($p->{lhs} eq $aug){$key='$';$val=['a',0]}
    else {$key=$la;$val=['r',0+$pid]}
    next unless defined $key;
    if(defined $a{$key} && join(':',@{$a{$key}}) ne join(':',@$val)){
      my $resolved=$resolve->($key,$a{$key},$val);
      if(!$resolved){my $details=join("; ",map {my $entry=$_;$entry->[0] eq 'r'?"reduce ".$prods[$entry->[1]]{lhs}." -> ".join(" ",@{$prods[$entry->[1]]{rhs}}):"shift $key"} ($a{$key},$val));fail("LR conflict ($algorithm; $details) in state $i on '$key': ".join(':',@{$a{$key}}).' versus '.join(':',@$val),$s->{file},1)}
      $val=$resolved;
    }
    $a{$key}=$val;
  }
  $action[$i]=\%a;$goto[$i]={map {$_=>$trans[$i]{$_}} grep {$nonterms{$_}} keys %{$trans[$i]||{}}};
 }
 for my $row(@action){$_->[1]=0+$_->[1] for values %$row}
 $s->{productions}=\@prods;$s->{actions}=\@action;$s->{gotos}=\@goto;
 $s->{terminals}=[sort keys %tokens];$s->{nonterminals}=\%nonterms;
}
sub compile_actions {
 my($s)=@_;my %funidx=map {$s->{functions}[$_]{name}=>$_} 0..$#{$s->{functions}};
 my %ops; for my $ir (@{$s->{irs}}){$ops{$ir->{name}.'.'.$_->{name}}={%$_,id=>scalar keys %ops} for @{$ir->{ops}}}
 $s->{op_by_name}=\%ops;
 for my $f(@{$s->{functions}}){$f->{code}=parse_action($f->{body},$s->{file},$f->{line});}
 $s->{init_code}=parse_action($s->{init},$s->{file},1);
 for my $r (@{$s->{rules}}){for my $key(qw(semantics lower analyze emit)){
    $r->{code}{$key}=parse_action($r->{facets}{$key},$s->{file},$r->{line}) if defined $r->{facets}{$key};
 }}
}
# Lex action-language into token classes with position-independent literal values.
sub atoks {
 my($s)=@_;my @out;
 $s//= '';
 while((pos($s)//0) < length($s)){
  if($s =~ /\G\s+/gc){next}
  if($s =~ /\G\#\s+(?:[^\n]*)/gc){next}
  if($s =~ /\G\/\/[^\n]*/gc){next}
  if($s =~ /\G(["'])( (?:\\.|[^\\])*? )\1/gcx){my $st=decode_string($2);push @out,{k=>'str',v=>$st};next}
  if($s =~ /\G(\d+(?:\.\d*)?(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?)/gc){push @out,{k=>'num',v=>$1};next}
  if($s =~ /\G([\@\#\$\!\%\&][A-Za-z_]\w*|t'[A-Za-z_]\w*|[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)?)/gc){push @out,{k=>'name',v=>$1};next}
  if($s =~ /\G(->|==|!=|>=|<=|&&|\|\||=>|[-+*\/=<>!;,(){}\[\]:.])/gc){push @out,{k=>'op',v=>$1};next}
  my $bad=substr($s,pos($s)//0,25);die "invalid action syntax near '$bad'\n";
 }
 push @out,{k=>'end',v=>'<end>'};return \@out;
}
sub apeek {return $_[0]{t}[$_[0]{i}]{v}}
sub anext {return $_[0]{t}[$_[0]{i}++]{v}}
sub aaccept {my($x,$v)=@_;if(apeek($x) eq $v){anext($x);return 1}return 0}
sub expect {my($x,$v)=@_;aaccept($x,$v) or die "expected '$v', got '".apeek($x)."' in semantic action\n"}
our %prec; BEGIN { %prec=('||'=>1,'&&'=>2,'in'=>3,'=='=>3,'!='=>3,'>'=>3,'<'=>3,'>='=>3,'<='=>3,'+'=>4,'-'=>4,'*'=>5,'/'=>5); }
sub expr {my($x,$min)=@_;$min//=0;my $t=$x->{t}[$x->{i}++];my $v=$t->{v};my $node;
 if($t->{k} eq 'num'){$node={k=>'num',v=>0+$v,literal=>"$v",type=>($v =~ /[.eE]/?'Float64':'Int')}}
 elsif($t->{k} eq 'str'){$node={k=>'str',v=>$v}}
 elsif($v eq '['){my @items;unless(aaccept($x,']')){do{push @items,expr($x,0)}while(aaccept($x,','));expect($x,']')}$node={k=>'list',args=>\@items}}
 elsif($v eq '('){$node=expr($x,0);expect($x,')')}
 elsif($v eq '-' || $v eq '!'){$node={k=>'un',v=>$v,a=>expr($x,6)};if($v eq '-' && $node->{a}{k} eq 'num'){$node={%{$node->{a}},v=>-$node->{a}{v},literal=>'-'.$node->{a}{literal}}}}
 elsif($t->{k} eq 'name'){$node={k=>'name',v=>$v}}
 else{die "unexpected '$v' in expression\n"}
 while(1){my $p=apeek($x);
  if($p eq '('){anext($x);my @args;unless(aaccept($x,')')){do{push @args,expr($x,0)}while(aaccept($x,','));expect($x,')')}$node={k=>'call',a=>$node,args=>\@args};next}
  if($p eq '['){anext($x);my $idx=expr($x,0);expect($x,']');$node={k=>'index',a=>$node,b=>$idx};next}
  if(exists($prec{$p}) && $prec{$p}>=$min){anext($x);my $r=expr($x,$prec{$p}+1);$node={k=>'bin',v=>$p,a=>$node,b=>$r};next}
  last;
 }return $node;
}
sub stmt_list {my($x,$until)=@_;my @list;while(apeek($x) ne '<end>' && (!defined $until || apeek($x) ne $until)){
  if(aaccept($x,';')){next}
  if(aaccept($x,'on')){
    my $name=anext($x);expect($x,'->');my $body=stmt_list($x,'->');expect($x,'->');expect($x,'end');push @list,{k=>'on',name=>$name,body=>$body};next;
  }
  if(aaccept($x,'require')){my $cond=expr($x,0);expect($x,'else');expect($x,'Throw');expect($x,'(');my $name=anext($x);my @args;
    while(aaccept($x,',')){push @args,expr($x,0)}expect($x,')');expect($x,';');push @list,{k=>'require',cond=>$cond,name=>$name,args=>\@args};next}
  if(aaccept($x,'let')){my $name=anext($x);my $type='';if(aaccept($x,':')){while(apeek($x) ne '=' && apeek($x) ne '<end>'){$type.=anext($x)}}expect($x,'=');my $rhs=expr($x,0);expect($x,';');push @list,{k=>'let',name=>$name,type=>$type,expr=>$rhs};next}
  if(aaccept($x,'grab')){my $key=expr($x,0);expect($x,'from');my $table=anext($x);expect($x,'into');my $typ=anext($x);expect($x,'(');my $var=anext($x);expect($x,')');expect($x,';');push @list,{k=>'grab',key=>$key,table=>$table,type=>$typ,var=>$var};next}
  if(aaccept($x,'call')){my $e=expr($x,0);expect($x,';');push @list,{k=>'exec',expr=>($e->{k} eq 'call'?$e:{k=>'call',a=>$e,args=>[]})};next}
  if(aaccept($x,'discard')){my $e=expr($x,0);expect($x,';');push @list,{k=>'exec',expr=>$e};next}
  if(aaccept($x,'return')){my $e=apeek($x) eq ';'?{k=>'name',v=>'nil'}:expr($x,0);expect($x,';');push @list,{k=>'return',expr=>$e};next}
  if(aaccept($x,'while')){my $cond=expr($x,0);expect($x,'{');my $body=stmt_list($x,'}');expect($x,'}');push @list,{k=>'while',cond=>$cond,body=>$body};next}
  if(aaccept($x,'for')){my $var=anext($x);expect($x,'in');my $items=expr($x,0);expect($x,'{');my $body=stmt_list($x,'}');expect($x,'}');push @list,{k=>'for',var=>$var,items=>$items,body=>$body};next}
  if(aaccept($x,'if')){my $c=expr($x,0);expect($x,'{');my $a=stmt_list($x,'}');expect($x,'}');my $b=[];if(aaccept($x,'else')){expect($x,'{');$b=stmt_list($x,'}');expect($x,'}')}push @list,{k=>'if',cond=>$c,yes=>$a,no=>$b};next}
  if(aaccept($x,'Throw')){expect($x,'(');my $name=anext($x);my @args;while(aaccept($x,',')){push @args,expr($x,0)}expect($x,')');expect($x,';');push @list,{k=>'throw',name=>$name,args=>\@args};next}
  my $e=expr($x,0);if(aaccept($x,'=')){my $value=expr($x,0);expect($x,';');push @list,{k=>'assign',lhs=>$e,rhs=>$value}}
  else{expect($x,';');push @list,{k=>'exec',expr=>$e}}
 }
 return \@list;
}
sub parse_action {my($code,$file,$line)=@_;my $t=atoks($code);my $x={t=>$t,i=>0};my $list=eval{stmt_list($x,undef)};if($@){fail("invalid semantic action: $@",$file,$line)}return $list;}
sub validate {my($s)=@_;
 my %names;for my $t(@{$s->{tokens}}){$names{$t->{name}}++ and fail("duplicate token $t->{name}",$s->{file},$t->{line});
  # Reject empty tokens at spec time. portable regex checks are strict.
  my $r=$t->{regex};$r =~ /\(\?[!=<>{]/ and fail("non-portable regex for token $t->{name}",$s->{file},$t->{line});
  my $q=eval {qr/$r/};$@ and fail("bad regexp $t->{name}: $@",$s->{file},$t->{line});
  '' =~ /$q/ and fail("empty-matching token $t->{name}",$s->{file},$t->{line});
 }
 for my $t(@{$s->{skips}}){my $q=eval {qr/$t->{regex}/};$@ and fail("bad skip regexp: $@",$s->{file},$t->{line});'' =~ /$q/ and fail('empty-matching skip pattern',$s->{file},$t->{line})}
}

# Table-driven lexer compiler: portable ASCII/UTF-8 byte NFA -> DFA.
sub rset {
 my(@bits)=@_;my %bit=map {$_=>1} @bits;
 my $str='0'x256;for my $i(keys %bit){next if $i<0||$i>255;substr($str,$i,1)='1'}return $str;
}
sub rpeek {my($p)=@_;return substr($p->{src},$p->{pos},1)}
sub rget {my($p)=@_;return substr($p->{src},$p->{pos}++,1)}
sub resc {
 my($p)=@_;my $e=rget($p);$e ne '' or die "truncated regex escape\n";
 if($e eq 'd'){return rset(48..57)}
 if($e eq 'w'){return rset(48..57,65..90,95,97..122)}
 if($e eq 's'){return rset(9,10,11,12,13,32)}
 if($e eq 'n'){return rset(10)}
 if($e eq 'r'){return rset(13)}
 if($e eq 't'){return rset(9)}
 if($e eq 'f'){return rset(12)}
 if($e eq 'v'){return rset(11)}
 if($e eq 'x') {my $h=substr($p->{src},$p->{pos},2);$h =~ /^[0-9a-fA-F]{2}$/ or die "invalid \\x hex escape\n";$p->{pos}+=2;return rset(hex($h))}
 $e !~ /[1-9pPAbBGGK]/ or die "unsupported backreference or character class '$e'\n";
 return rset(ord($e));
}
sub r_charclass {
 my($p)=@_;my $neg=rpeek($p) eq '^'?rget($p):'';
 my @sets;
 while($p->{pos}<length $p->{src} && rpeek($p) ne ']'){
  my $ch=rget($p);my $first=$ch eq '\\'?resc($p):rset(ord($ch));
  # 'x-y' is a range only when x and y are singletons.
  if(rpeek($p) eq '-' && substr($p->{src},$p->{pos}+1,1) ne ']' && $p->{pos}+1<length($p->{src})){
   rget($p);my $c=rget($p);my $last=$c eq '\\'?resc($p):rset(ord($c));
   my @fa=grep {substr($first,$_,1) eq '1'} 0..255;
   my @fb=grep {substr($last,$_,1) eq '1'} 0..255;
   @fa==1&&@fb==1&&$fa[0]<=$fb[0] or die "invalid character-class range\n";
   $first=rset($fa[0]..$fb[0]);
  }
  push @sets,$first;
 }
 rget($p) eq ']' or die "unterminated character class\n";
 my $chars='0'x256;for my $str(@sets){for(my $i=0;$i<256;$i++){substr($chars,$i,1)='1' if substr($str,$i,1) eq '1'}}
 if($neg){$chars =~ tr/01/10/}
 return {k=>'set',chars=>$chars};
}
sub r_alt {my($p)=@_;my @alts=(r_seq($p));while(rpeek($p) eq '|'){rget($p);push @alts,r_seq($p)}return @alts==1?$alts[0]:{k=>'alt',v=>\@alts}}
sub r_seq {my($p)=@_;my @seq;while($p->{pos}<length $p->{src} && rpeek($p) ne ')' && rpeek($p) ne '|'){push @seq,r_repeat($p)}return @seq==1?$seq[0]:{k=>'seq',v=>\@seq}}
sub r_repeat {my($p)=@_;my $ch=rget($p);my $node;
 if($ch eq '('){
   if(rpeek($p) eq '?'){rget($p);rget($p) eq ':' or die "only noncapturing (?:...) groups are supported\n"}
   $node=r_alt($p);rget($p) eq ')' or die "unterminated regex group\n";
 }elsif($ch eq '['){$node=r_charclass($p)}
 elsif($ch eq '.'){my $bits='1'x256;substr($bits,10,1)='0';$node={k=>'set',chars=>$bits}}
 elsif($ch eq '\\'){$node={k=>'set',chars=>resc($p)}}
 elsif($ch eq '*'||$ch eq '+'||$ch eq '?'||$ch eq '{'||$ch eq '}'){die "unexpected quantifier '$ch'\n"}
 else{$node={k=>'set',chars=>rset(ord($ch))}}
 my $next=rpeek($p);
 if($next eq '*'||$next eq '+'||$next eq '?'){rget($p);$node={k=>$next,v=>$node}}
 elsif($next eq '{'){
  my $tail=substr($p->{src},$p->{pos});
  $tail =~ /^\{(\d+)(?:,(\d*)?)?\}/ or die "invalid bounded repetition\n";
  my ($lo,$hi)=($1,defined($2)?($2 eq ''?32:$2):$1);
  $lo<=32&&$hi<=32&&$hi>=$lo or die "repetition bounds must satisfy 0<=min<=max<=32\n";
  $p->{pos}+=length($&);$node={k=>'rep',v=>$node,lo=>$lo,hi=>$hi};
 }
 return $node;
}
sub regex_ast {
 my($raw)=@_;my $begin=0,my $end=0;
 if($raw =~ /^\^/){$begin=1;substr($raw,0,1)=''}
 if($raw =~ /(?<!\\)\$$/){$end=1;substr($raw,-1,1)=''}
 my $p={src=>$raw,pos=>0};my $ast=r_alt($p);
 $p->{pos}==length($raw) or die "regex parse stopped at ".substr($raw,$p->{pos})."\n";
 return ($ast,$begin,$end);
}
sub compile_lexer {
 my($s)=@_;
 my @rules=((map {{%$_,skip=>0}} @{$s->{tokens}}), (map {{%$_,skip=>1}} @{$s->{skips}}));
 @rules=sort {($a->{line}//0)<=>($b->{line}//0)||($a->{skip}//0)<=>($b->{skip}//0)} @rules;
 # synthetic literal tokens precede generic token regex on equal-length matches.
 @rules=sort {($a->{line}==0?0:1)<=>($b->{line}==0?0:1)||($a->{line}<=>$b->{line})||($a->{skip}<=>$b->{skip})} @rules;
 my @nfa;my $new=sub {push @nfa,{eps=>[],trans=>[],accept=>[]};return $#nfa};
 my $edge=sub {my($src,$dst)=@_;push @{$nfa[$src]{eps}},$dst};
 my $mk; $mk=sub {my($a)=@_;my $k=$a->{k};my($start,$end)=($new->(),$new->());
  if($k eq 'set'){push @{$nfa[$start]{trans}},[$a->{chars},$end]}
  elsif($k eq 'seq'){my $prev=$start;for my $child(@{$a->{v}}){my($x,$y)=$mk->($child);$edge->($prev,$x);$prev=$y}$edge->($prev,$end)}
  elsif($k eq 'alt'){for my $child(@{$a->{v}}){my($x,$y)=$mk->($child);$edge->($start,$x);$edge->($y,$end)}}
  elsif($k eq '*'){my($x,$y)=$mk->($a->{v});$edge->($start,$end);$edge->($start,$x);$edge->($y,$end);$edge->($y,$x)}
  elsif($k eq '+'){my($x,$y)=$mk->($a->{v});$edge->($start,$x);$edge->($y,$end);$edge->($y,$x)}
  elsif($k eq '?'){my($x,$y)=$mk->($a->{v});$edge->($start,$end);$edge->($start,$x);$edge->($y,$end)}
  elsif($k eq 'rep'){
   my $last=$start;for my $i(1..$a->{hi}){my($x,$y)=$mk->($a->{v});if($i>$a->{lo}){$edge->($last,$end)}$edge->($last,$x);$last=$y}
   $edge->($last,$end);
  }else{die "unknown regex node $k\n"}return ($start,$end);
 };
 my $root=$new->();
 for my $i(0..$#rules){my $r=$rules[$i];my($ast,$bol,$eol)=regex_ast(Encode::encode('UTF-8',$r->{regex}));$r->{bol}=$bol;$r->{eol}=$eol;
   my($st,$en)=$mk->($ast);$edge->($root,$st);push @{$nfa[$en]{accept}},$i;
   @nfa<5000 or die "NFA has too many states (>5000)\n";
 }
 my $closure=sub {my(@queue)=@_;my %seen=map {$_=>1} @queue;
  for(my $i=0;$i<@queue;$i++){for my $dst(@{$nfa[$queue[$i]]{eps}}){unless($seen{$dst}++){push @queue,$dst}}}
  return [sort {$a<=>$b} @queue];
 };
 my $start=$closure->($root);my (@states,@rows);@states=($start);my %ids=(join(',',@$start)=>0);
 for(my $id=0;$id<@states;$id++){
  my $set=$states[$id];my %acc;
  for my $src(@$set){$acc{$_}=1 for @{$nfa[$src]{accept}}}
  my @next;
  for my $b(0..255){my %dst;
   for my $src (@$set){for my $edge(@{$nfa[$src]{trans}}){$dst{$edge->[1]}=1 if substr($edge->[0],$b,1) eq '1'}}
   if(%dst){my $closure=$closure->(keys %dst);my $key=join(',',@$closure);if(!exists $ids{$key}){$ids{$key}=@states;push @states,$closure}
     $next[$b]=$ids{$key};
   }else{$next[$b]=-1}
  }
  my @ranges;my ($lo,$prev,$dest)=(0,0,$next[0]);
  for my $b(1..256){my $now=$b==256?undef:$next[$b];
   if(!defined($now)||$now!=$dest){push @ranges,[$lo,$b-1,$dest] if $dest>=0;$lo=$b;$dest=$now;}
  }
  $rows[$id]={accept=>[sort {$a<=>$b} keys %acc],trans=>\@ranges};
  @states<4096 or die "DFA has too many states (>4096)\n";
 }
 $s->{lexdfa}={rules=>[map {{name=>$_->{name}||'',type=>$_->{type}||'String',skip=>$_->{skip},bol=>$_->{bol},eol=>$_->{eol},regex=>$_->{regex}}} @rules],states=>\@rows};
}
1;

# Declarative IR rewrite and backend-template preparation.
sub comma_parts {
 my($s)=@_;return () unless length strip_ws($s);my @out;my($depth,$quote,$escape,$last)=(0,'',0,0);
 for(my $i=0;$i<length($s);$i++){
  my $c=substr($s,$i,1);
  if($quote){if($escape){$escape=0}elsif($c eq '\\'){$escape=1}elsif($c eq $quote){$quote=''}next}
  if($c eq '"'||$c eq "'"){$quote=$c;next}
  if($c eq '(' || $c eq '['){$depth++}elsif($c eq ')' || $c eq ']'){$depth--}
  elsif($c eq ',' && $depth==0){push @out,strip_ws(substr($s,$last,$i-$last));$last=$i+1}
 }
 push @out,strip_ws(substr($s,$last));return @out;
}
sub expr_snippet {my($s)=@_;my $x={t=>atoks($s),i=>0};my $e=expr($x,0);apeek($x) eq '<end>' or die "trailing tokens in expression '$s'\n";return $e}
sub unquote_template {my($s)=@_;$s=strip_ws($s);$s =~ /^"(.*)"$/s or die "expected double-quoted template, got '$s'\n";
 return decode_string($1);
}
sub parse_opcode_call {
 my($text,$ir,$exprs)=@_;$text=strip_ws($text);
 $text =~ /^(\w+)(?:\((.*)\))?$/s or die "invalid IR operation syntax '$text'\n";
 my($name,$raw)=($1,$2);my @args=defined($raw) && length(strip_ws($raw)) ? comma_parts($raw) : ();
 my @expr=$exprs?map {expr_snippet($_)} @args:@args;
 return {op=>$ir.'.'.$name,args=>\@expr};
}
sub compile_passes {
 my($s)=@_;
 $s->{rewrite_fuel}=10000;$s->{rewrite_repeat}=1;
 @{$s->{pipelines}}>1 and fail('only one pipeline can be selected in this release',$s->{file},1);
 for my $p(@{$s->{pipelines}}){my $stages='';
  for my $line(split /\n/,$p->{body}){my $l=strip_ws($line);next unless length $l;
   if($l=~/^\.fuel\s+optimize\s*=\s*(\d+)$/){$1>0 && $1<=1000000 or fail('rewrite fuel must be 1..1000000',$s->{file},1);$s->{rewrite_fuel}=0+$1}
   elsif($l=~/^\.on_cycle\s+error$/){}
   elsif($l=~/^\./){fail("unsupported pipeline directive '$l'",$s->{file},1)}
   else{$stages.=' '.$l}
  }
  $stages=~s/\s+//g;
  $stages=~/^parse->lower(?:->(optimize\*?))?->verify->emit$/ or fail('stack pipelines require parse -> lower -> [optimize or optimize* ->] verify -> emit',$s->{file},1);
  $s->{rewrite_enabled}=defined $1?1:0;$s->{rewrite_repeat}=defined($1)&&$1 eq 'optimize*'?1:0;
 }
 $s->{rewrite_enabled}=1 unless defined $s->{rewrite_enabled};
 for my $state (@{$s->{states}}){$state->{initial_expr}=expr_snippet($state->{initial});}
 my @rewrites;
 for my $r(@{$s->{rewrites}}){
  $r->{name} =~ /^(\w+)\s*:\s*(\w+)$/ or fail("invalid rewrite declaration '$r->{name}'",$s->{file},1);
  my ($name,$ir)=($1,$2);
  my $body=$r->{body};my(%parts,$priority);
  for my $line(split /\n/,$body){my $l=strip_ws($line);next if !$l || $l =~ /^#(?:\s|$)/;
    if($l =~ /^\.(phase|priority|match|when|replace)\s+(.+)$/){$parts{$1}=$2;next}
    fail("unknown rewrite directive '$l'",$s->{file},1);
  }
  (!defined($parts{phase}) || $parts{phase} eq 'optimize') or fail('only optimize-phase stack rewrites are supported',$s->{file},1);
  $parts{match} && $parts{replace} or fail("rewrite '$name' needs .match and .replace",$s->{file},1);
  my @match=map {parse_opcode_call($_,$ir,0)} comma_parts($parts{match});
  my @replace=map {parse_opcode_call($_,$ir,1)} comma_parts($parts{replace});
  my $when=defined $parts{when}?expr_snippet($parts{when}):{k=>'num',v=>1};
  for my $op (@match,@replace){$s->{op_by_name}{$op->{op}} or fail("rewrite '$name' references undeclared IR opcode $op->{op}",$s->{file},1)}
  push @rewrites,{name=>$name,ir=>$ir,phase=>$parts{phase}||'optimize',priority=>0+($parts{priority}||0),match=>\@match,replace=>\@replace,when=>$when};
 }
 $s->{rewrites_compiled}=[sort {$b->{priority}<=>$a->{priority} || $a->{name} cmp $b->{name}} @rewrites];
 my @backends;
 for my $r(@{$s->{backends}}){$r->{name}=~/^(\w+)\s+for\s+(\w+)$/ or fail("invalid backend declaration '$r->{name}'",$s->{file},1);
  my($target,$ir)=($1,$2);my %emits;my($prologue,$epilogue)=('','');
  for my $line(split /\n/,$r->{body}){my $l=strip_ws($line);next if !$l || $l =~ /^#(?:\s|$)/;
   if($l =~ /^\.emit\s+(\w+)(?:\(([^)]*)\))?\s*=>\s*(".*")\s*$/){
    my($op,$args,$value)=($1,$2,$3);
    $s->{op_by_name}{$ir.'.'.$op} or fail("backend '$target' refers to undeclared opcode $ir.$op",$s->{file},1);
    $emits{$op}={params=>[grep {length} map {strip_ws($_)} split /,/,$args||''],template=>unquote_template($value)};next;
   }
   if($l =~ /^\.prologue\s+(".*")$/){$prologue=unquote_template($1);next}
   if($l =~ /^\.epilogue\s+(".*")$/){$epilogue=unquote_template($1);next}
   fail("unknown backend directive '$l'",$s->{file},1);
  }
  for my $op(grep {index($_,$ir.'.')==0} sort keys %{$s->{op_by_name}}){
   (my $short=$op)=~s/^\Q$ir\E\.//;
   exists $emits{$short} or fail("backend '$target' has no mapping for $op",$s->{file},1);
   my @params=comma_parts($s->{op_by_name}{$op}{params});
   @params==@{$emits{$short}{params}} or fail("backend '$target' operand count mismatch for $op",$s->{file},1);
   my %params=map {$_=>1} @{$emits{$short}{params}};
   while($emits{$short}{template}=~/\$\{(\w+)\}/g){$params{$1} or fail("unknown backend template parameter '$1'",$s->{file},1)}
  }
  ($prologue.$epilogue)=~/\$\{/ and fail('backend prologue/epilogue cannot interpolate operands',$s->{file},1);
  push @backends,{target=>$target,ir=>$ir,emits=>\%emits,prologue=>$prologue,epilogue=>$epilogue};
 }
 $s->{backends_compiled}=\@backends;
}
1;

# Static name binding, exception-arity, capture binding and lexical checks.
sub validate_actions {
 my($s,$mode)=@_;
 my %state=map {$_->{name}=>$_->{type}} @{$s->{states}};
 my %table=map {$_->{name}=>$_->{type}} @{$s->{tables}};
 my %func=map {$_->{name}=>$_} @{$s->{functions}};
 my %ex=map {$_->{name}=>$_} @{$s->{exceptions}};
 my %ops=%{$s->{op_by_name}};
 my %builtins=map {$_=>1} qw(len pop top push emit print final_stack_height reverse min max abs is_finite);
 my %arity=(len=>[1,1],pop=>[1,1],top=>[1,1],push=>[2,2],emit=>[2,2],print=>[1,1],final_stack_height=>[1,1],reverse=>[1,1], min=>[1,2],max=>[1,2],abs=>[1,1],is_finite=>[1,1]);
 my $diag=sub {my($msg,$line)=@_;fail($msg,$s->{file},$line||1)};
 my $checkexpr;
 $checkexpr=sub {my($e,$env,$captures,$line)=@_;return unless $e;
  my $kind=$e->{k};
  if($kind eq 'name'){
   my $name=$e->{v};
   if($name =~ /^\@(\w+)$/){$diag->("undeclared state '$name'",$line) unless $state{$1}}
   elsif($name =~ /^\#(\w+)$/){$diag->("undeclared symbol table '$name'",$line) unless $table{$1}}
   elsif($name =~ /^\$(\w+)$/){$diag->("unbound capture '$name'",$line) unless exists $captures->{$1}}
   elsif($name =~ /^&(\w+)$/){$diag->("undeclared function '$name'",$line) unless $func{$1}}
   elsif($name =~ /^t'(\w+)$/){$diag->("undeclared temporary '$name'",$line) unless $env->{$name}}
   elsif($name =~ /\./){$diag->("unknown IR operation '$name'",$line) unless $ops{$name}}
   elsif(!($env->{$name}||$builtins{$name}||$func{$name}||$name =~ /^(?:true|false|nil|None)$/)){
    $diag->("undeclared variable '$name'",$line);
   }
  }
  elsif($kind eq 'bin'||$kind eq 'index'){$checkexpr->($e->{a},$env,$captures,$line);$checkexpr->($e->{b},$env,$captures,$line)}
  elsif($kind eq 'un'){$checkexpr->($e->{a},$env,$captures,$line)}
  elsif($kind eq 'list'){for my $a(@{$e->{args}}){$checkexpr->($a,$env,$captures,$line)}}
  elsif($kind eq 'call'){
   $checkexpr->($e->{a},$env,$captures,$line);
   $checkexpr->($_,$env,$captures,$line) for @{$e->{args}};
   my $callee=$e->{a};my $name=$callee->{k} eq 'name'?$callee->{v}:'';
   if($arity{$name}){my($min,$max)=@{$arity{$name}};my $found=@{$e->{args}};
    $diag->("'$name' expects $min..$max operands, got $found",$line) if $found<$min||$found>$max;
   }
   if($ops{$name}){my $params=$ops{$name}{params}//'';my @params=grep {length strip_ws($_)} split /,/,$params;
    $diag->("IR constructor '$name' expects ".scalar(@params)." operands, got ".scalar(@{$e->{args}}),$line) unless @params==@{$e->{args}};
   }
  }
 };
 my $checkexc=sub {my($stmt,$env,$caps,$line)=@_;
  my $name=$stmt->{name};$name =~ s/^!//;
  $diag->("undeclared exception '!$name'",$line) unless exists $ex{$name};
  my @args=grep {length strip_ws($_)} split /,/,$ex{$name}{params};
  $diag->("exception '!$name' expects ".scalar(@args)." operands",$line) unless @args==@{$stmt->{args}};
  for my $a(@{$stmt->{args}}){$checkexpr->($a,$env,$caps,$line)};
 };
 my $checkblock;
 $checkblock=sub {my($block,$env,$caps,$avail,$line)=@_;
  for my $st(@$block){my $k=$st->{k};
   if($k eq 'on'){
    $diag->("on-branch capture '$st->{name}' not present in rule",$line) unless $avail->{$st->{name}};
    my %newcaps=%$caps;$newcaps{$_}=1 for keys %{$avail->{$st->{name}}};
    $checkblock->($st->{body},{%$env},\%newcaps,$avail,$line);
   }
   elsif($k eq 'require'){$checkexpr->($st->{cond},$env,$caps,$line);$checkexc->($st,$env,$caps,$line)}
   elsif($k eq 'throw'){$checkexc->($st,$env,$caps,$line)}
   elsif($k eq 'let'){$checkexpr->($st->{expr},$env,$caps,$line);$env->{$st->{name}}=1}
   elsif($k eq 'grab'){$checkexpr->($st->{key},$env,$caps,$line);$st->{table}=~/^\#(\w+)$/&&$table{$1} or $diag->("unknown table in grab",$line);$env->{$st->{var}}=1}
   elsif($k eq 'return'||$k eq 'exec'){$checkexpr->($st->{expr},$env,$caps,$line)}
   elsif($k eq 'assign'){
    my $lhs=$st->{lhs};
    $lhs->{k} eq 'name' || $lhs->{k} eq 'index' or $diag->('invalid assignment target',$line);
    if($lhs->{k} eq 'name'){$env->{$lhs->{v}}=1 unless $lhs->{v}=~/^\@/}
    else {$checkexpr->($lhs->{a},$env,$caps,$line);$checkexpr->($lhs->{b},$env,$caps,$line)}
    $checkexpr->($st->{rhs},$env,$caps,$line);
   }
   elsif($k eq 'while'){$checkexpr->($st->{cond},$env,$caps,$line);$checkblock->($st->{body},{%$env},$caps,$avail,$line)}
   elsif($k eq 'for'){$checkexpr->($st->{items},$env,$caps,$line);$checkblock->($st->{body},{%$env,$st->{var}=>1},$caps,$avail,$line)}
   elsif($k eq 'if'){$checkexpr->($st->{cond},$env,$caps,$line);$checkblock->($st->{yes},{%$env},$caps,$avail,$line);$checkblock->($st->{no},{%$env},$caps,$avail,$line)}
  }
 };
 my %initialized;
 for my $state(@{$s->{states}}){
  $checkexpr->($state->{initial_expr},{},{},1);
  my %visited;my $dependencies;$dependencies=sub{my($node)=@_;return unless ref $node;
   if(ref $node eq 'ARRAY'){$dependencies->($_) for @$node;return}
   return unless ref $node eq 'HASH';
   if(($node->{k}||'') eq 'name'){
    my $name=$node->{v};
    if($name=~/^@(\w+)$/ && !$initialized{$1}){$diag->("state initialization order: \@$state->{name} reads $name before initialization",1)}
    if($name=~/^#/){$diag->('state initializers cannot access tables before .init',1)}
   }
   if(($node->{k}||'') eq 'call' && $node->{a}{k} eq 'name'){
    my $name=$node->{a}{v};$name=~s/^&//;
    $dependencies->($func{$name}{code}) if exists($func{$name}) && !$visited{$name}++;
   }
   $dependencies->($_) for grep {ref} values %$node;
  };
  $dependencies->($state->{initial_expr});$initialized{$state->{name}}=1;
 }
 $checkblock->($s->{init_code},{},{},{},1);
 for my $f(@{$s->{functions}}){my %params=map {(split /:/,strip_ws($_))[0]=>1} grep {length strip_ws($_)} split /,/,$f->{args};$checkblock->($f->{code},\%params,{},{},$f->{line})}
 for my $r(@{$s->{rules}}){my (%available,%always);my $first=1;
  for my $p(@{$s->{productions}}){next unless $p->{lhs} eq $r->{name};
   my %here=map {$_=>1} grep {$_ ne ''} @{$p->{labels}};
   for my $label(keys %here){if(!exists $available{$label}){$available{$label}={%here}}else{delete $available{$label}{$_} for grep {!$here{$_}} keys %{$available{$label}}}}
   if($first){%always=%here;$first=0}else{delete $always{$_} for grep {!$here{$_}} keys %always;}
  }
  for my $key(qw(semantics lower analyze emit)){
   $checkblock->($r->{code}{$key},{},\%always,\%available,$r->{line}) if $r->{code}{$key};
  }
 }
 for my $ex(@{$s->{exceptions}}){my %names=map {strip_ws((split /:/,$_)[0])=>1} grep {length strip_ws($_)} split /,/,$ex->{params};
  while($ex->{message}=~/\{(\w+)\}/g){$diag->("message of !$ex->{name} contains undeclared argument '$1'",1) unless $names{$1}}
 }
}
1;

sub validate_program_backend {
 my($s)=@_;return unless $opt{mode} =~ /^(both|compile|rewrite)$/;
 my $target=$opt{program_target};return if grep {$_->{target} eq $target} @{$s->{backends_compiled}};
 $target =~ /^(c99|c11|cpp17|cpp20|python3|bytecode)$/ or fail("no backend defined for program target '$target'",$s->{file},1);
 return if $target eq 'bytecode';
 my %shape=(PUSH_F64=>[0,1,1],ADD=>[2,1,0],SUB=>[2,1,0],MUL=>[2,1,0],DIV=>[2,1,0],NEG=>[1,1,0],DUP=>[1,2,0],SWAP=>[2,2,0],DROP=>[1,0,0],PRINT=>[1,1,0]);
 for my $name(sort keys %{$s->{op_by_name}}){my $op=$s->{op_by_name}{$name};(my $short=$name)=~s/.*\.//;
  my $expected=$shape{$short};my @params=comma_parts($op->{params});
  $expected && $op->{pop}==$expected->[0] && $op->{push}==$expected->[1] && @params==$expected->[2]
   && !(grep {$_ ne 'Float64'} (@{$op->{inputs}},@{$op->{outputs}}))
   && !(grep {!/:\s*Float64\s*$/} @params)
   or fail("no built-in $target lowering for $name with this signature; define a .backend",$s->{file},1);
 }
}

sub validate_rewrites {
 my($s)=@_;my %ops=%{$s->{op_by_name}};
 my $signature=sub{my($seq)=@_;my(@required,@stack);
  for my $item(@$seq){my $decl=$ops{$item->{op}};my @in=@{$decl->{inputs}};
   if(@stack<@in){my @missing=splice(@in,0,@in-@stack);unshift @required,@missing}
   my @actual=splice(@stack,@stack-@in,scalar @in);
   join(',',@actual) eq join(',',@in) or fail('IR type mismatch within rewrite sequence',$s->{file},1);
   push @stack,@{$decl->{outputs}};
  }
  return join(',',@required).'->'.join(',',@stack);
 };
 for my $r(@{$s->{rewrites_compiled}}){
  my %bindings=(fp_mode=>1);my %arity=(is_finite=>1,abs=>1,min=>2,max=>2,len=>1);
  for my $op(@{$r->{match}},@{$r->{replace}}){my @params=comma_parts($ops{$op->{op}}{params});
   @params==@{$op->{args}} or fail("wrong operand count in rewrite $r->{name}: $op->{op}",$s->{file},1);
  }
  for my $op(@{$r->{match}}){$bindings{$_}=1 for grep {/^[A-Za-z_]\w*$/} @{$op->{args}}}
  my $pure;$pure=sub{my($e)=@_;return unless $e;
   if($e->{k} eq 'name'){$bindings{$e->{v}} || $e->{v}=~/^(true|false)$/ or fail("unbound or effectful rewrite expression '$e->{v}'",$s->{file},1)}
   elsif($e->{k} eq 'call'){my $name=$e->{a}{v}||'';$arity{$name} or fail("rewrite calls non-pure function '$name'",$s->{file},1);
    @{$e->{args}}==$arity{$name} or fail("wrong argument count for $name in rewrite",$s->{file},1);$pure->($_) for @{$e->{args}};
   }else{$pure->($e->{$_}) for grep {ref $e->{$_} eq 'HASH'} keys %$e;$pure->($_) for @{$e->{args}||[]}}
  };
  $pure->($r->{when});for my $op(@{$r->{replace}}){$pure->($_) for @{$op->{args}}}
  my $effects=sub{my($seq)=@_;return join(',',map {@{$ops{$_->{op}}{effects}||[]}} @$seq)};
  $effects->($r->{match}) eq $effects->($r->{replace}) or fail("rewrite $r->{name} changes observable IR effects",$s->{file},1);
  $signature->($r->{match}) eq $signature->($r->{replace}) or fail("rewrite $r->{name} changes its stack type signature",$s->{file},1);
 }
}

# Types are checked on the same action IR consumed by every runtime.
sub validate_types {
 my($s)=@_;
 my %alias=map {('%'.$_->{name})=>$_->{type}} @{$s->{types}||[]};
 my $norm;$norm=sub{my($t,$seen)=@_;$t//= 'Any';$t=~s/\s+//g;
  $t='Int' if $t eq 'Int64';
  if(exists $alias{$t}){$seen||={};$seen->{$t}++ and fail("cyclic type alias $t",$s->{file},1);return $norm->($alias{$t},$seen)}
  return 'Vec<'.$norm->($1,$seen).'>' if $t=~/^Vec<(.+)>$/;
  return $t;
 };
 my $known;$known=sub{my($t)=@_;$t=$norm->($t);
  return 1 if $t =~ /^(Int|Float64|Bool|String|Void|Any)$/;
  return $known->($1) if $t =~ /^Vec<(.+)>$/;
  return 1 if $t =~ /^(\w+)\.Op$/ && grep {$_->{name} eq $1} @{$s->{irs}};
  if($t =~ /^Fn\((.*)\)->(.+)$/){return 0 unless $known->($2);for my $arg(comma_parts($1)){return 0 unless $known->($arg)}return 1}
  return 0;
 };
 my $type=sub{my($t)=@_;$known->($t) or fail("unsupported or unknown type '$t'",$s->{file},1);return $norm->($t)};
 my $compatible=sub{my($want,$got)=map {$norm->($_)} @_;return 1 if $want eq $got || $got eq 'Any' || $want eq 'Any';
  return 1 if $want eq 'Float64' && $got eq 'Int';
  return 1 if $want =~ /^Vec</ && $got eq 'Vec<Any>';
  return 0;
 };
 my $must=sub{my($want,$got,$where)=@_;$compatible->($want,$got) or fail("type mismatch in $where: expected $want, got $got",$s->{file},1)};
 my %states=map {$_->{name}=>$type->($_->{type})} @{$s->{states}};
 my %tables;for my $t(@{$s->{tables}}){$t->{type}=~/^\s*(.*?)\s*->\s*(.+)$/ or fail("table $t->{name} requires key and value types",$s->{file},1);$tables{$t->{name}}=[$type->($1),$type->($2)];}
 my %functions;for my $f(@{$s->{functions}}){my @params;
  for my $arg(comma_parts($f->{args})){my($name,$t)=split /\s*:\s*/,$arg,2;defined $t or fail('function parameter requires a type',$s->{file},$f->{line});push @params,[$name,$type->($t)];}
  $functions{$f->{name}}={params=>\@params,returns=>$type->($f->{returns})};
 }
 my %ops=%{$s->{op_by_name}};
 my %tokens=map {$_->{name}=>$type->($_->{type})} @{$s->{tokens}};
 my %exceptions;for my $ex(@{$s->{exceptions}}){$exceptions{'!'.$ex->{name}}=[map {my($n,$t)=split /\s*:\s*/,$_,2;$type->($t)} comma_parts($ex->{params})];}
 for my $op(values %ops){$type->($_) for @{$op->{inputs}},@{$op->{outputs}};for my $arg(comma_parts($op->{params})){my($n,$t)=split /\s*:\s*/,$arg,2;$type->($t);}}
 my $infer;
 $infer=sub{my($e,$env,$caps)=@_;return 'Void' unless $e;my $k=$e->{k};my $v=$e->{v}//'';
  if($k eq 'num'){if(($e->{type}||'') eq 'Int'){my $n=Math::BigInt->new($e->{literal});($n>=Math::BigInt->new('-9223372036854775808') && $n<=Math::BigInt->new('9223372036854775807')) or fail('Int literal out of signed 64-bit range',$s->{file},1)}if(($e->{type}||'') eq 'Float64' && !POSIX::isfinite($e->{v})){fail('Float64 literal is out of range',$s->{file},1)}return $e->{type}||'Float64';}return 'String' if $k eq 'str';
  if($k eq 'list'){my @ts=map {$infer->($_,$env,$caps)} @{$e->{args}};my $t=@ts?$ts[0]:'Any';$must->($t,$_,'list') for @ts;return "Vec<$t>";}
  if($k eq 'name'){
   return $states{$1} if $v =~ /^@(\w+)/;
   return 'Map<'.join(',',@{$tables{$1}}).'>' if $v =~ /^#(\w+)/;
   return $caps->{$1}||'Any' if $v =~ /^\$(\w+)/;
   return 'Bool' if $v =~ /^(true|false)$/;
   return 'Void' if $v =~ /^(None|nil)$/;
   if($ops{$v}){(my $ir=$v)=~s/\.[^.]+$/.Op/;return $ir}
   $v=~s/^&//;
   if(my $f=$functions{$v}){return 'Fn('.join(',',map {$_->[1]} @{$f->{params}}).')->'.$f->{returns}}
   return $env->{$v}||'Any';
  }
  if($k eq 'index'){
   my $a=$infer->($e->{a},$env,$caps);my $b=$infer->($e->{b},$env,$caps);
   if($a =~ /^Vec<(.+)>$/){$must->('Int',$b,'vector index');return $1}
   if($a =~ /^Map<(.*)>$/){my($key,$value)=comma_parts($1);$must->($key,$b,'table index');return $value}
   fail("indexing requires a vector or table, got $a",$s->{file},1) unless $a eq 'Any';return 'Any';
  }
  if($k eq 'un'){my $a=$infer->($e->{a},$env,$caps);$must->('Bool',$a,'logical negation') if $v eq '!';return $v eq '!'?'Bool':$a}
  if($k eq 'bin'){
   my $a=$infer->($e->{a},$env,$caps);my $b=$infer->($e->{b},$env,$caps);
   if($v eq 'in'){if($b =~ /^Map<(.*)>$/){my($key)=comma_parts($1);$must->($key,$a,'membership')}elsif($b =~ /^Vec<(.+)>$/){$must->($1,$a,'membership')}else{fail('membership requires a table or vector',$s->{file},1)}return 'Bool'}
   if($v =~ /^(?:&&|\|\|)$/){$must->('Bool',$_,'logical expression') for ($a,$b);return 'Bool'}
   if($v =~ /^(?:==|!=)$/){$must->($a,$b,'comparison') unless $compatible->($b,$a);return 'Bool'}
   for($a,$b){/^(?:Int|Float64|Any)$/ or fail("operator $v requires numeric operands, got $_",$s->{file},1)}
   return 'Bool' if $v =~ /^(?:<|>|<=|>=)$/;
   my $t=$a eq 'Float64'||$b eq 'Float64'?'Float64':$a eq 'Any'||$b eq 'Any'?'Any':'Int';$e->{type}=$t;return $t;
  }
  if($k eq 'call'){
   my $name=$e->{a}{k} eq 'name'?$e->{a}{v}:'';my @args=map {$infer->($_,$env,$caps)} @{$e->{args}};
   if($ops{$name}){my @params=map {(split /\s*:\s*/,$_,2)[1]} comma_parts($ops{$name}{params});for my $i(0..$#params){$must->($params[$i],$args[$i],"IR operand $name")}(my $t=$name)=~s/\.[^.]+$/.Op/;return $t}
   if($name =~ /^(push|emit|pop|top|reverse|len|final_stack_height)$/){
    my $vec=$args[0]||'';return 'Any' if $vec eq 'Any';if($name eq 'len'&&$vec =~ /^(?:String|Map<)/){return 'Int'}
    $vec =~ /^Vec<(.+)>$/ or fail("$name requires a vector, got $vec",$s->{file},1);my $elem=$1;
    if($name eq 'push'||$name eq 'emit'){$must->($elem,$args[1],$name);return 'Void'}
    return 'Int' if $name eq 'len'||$name eq 'final_stack_height';return $vec if $name eq 'reverse';return $elem;
   }
   return 'Void' if $name eq 'print';
   if($name =~ /^(abs|min|max|is_finite)$/){$must->('Float64',$_,$name) for @args;return $name eq 'is_finite'?'Bool':(grep {$_ eq 'Float64'} @args)?'Float64':$args[0]}
   my $callee=$infer->($e->{a},$env,$caps);
   $callee =~ /^Fn\((.*)\)->(.+)$/ or fail("expression is not callable ($callee)",$s->{file},1);
   my($params,$ret)=($1,$2);my @params=comma_parts($params);
   @params==@args or fail("function $name expects ".scalar(@params).' arguments, got '.scalar(@args),$s->{file},1);
   $must->($params[$_],$args[$_],"function $name") for 0..$#params;return $ret;
  }
  fail("unsupported expression $k",$s->{file},1);
 };
 my $observed_returns;my $block;$block=sub{my($code,$env,$caps,$ret)=@_;my $returns=0;
  for my $st(@$code){my $k=$st->{k};
   if($k eq 'let'){exists $env->{$st->{name}} and fail("duplicate local $st->{name}",$s->{file},1);my $got=$infer->($st->{expr},$env,$caps);my $t=$st->{type}?$type->($st->{type}):$got;$must->($t,$got,'local initializer');$env->{$st->{name}}=$t;}
   elsif($k eq 'return'){my $got=$infer->($st->{expr},$env,$caps);$must->($ret,$got,'return') if defined $ret;push @$observed_returns,$got if defined $observed_returns;$returns=1;}
   elsif($k eq 'exec'){$infer->($st->{expr},$env,$caps)}
   elsif($k eq 'assign'){my $lhs=$st->{lhs};if($lhs->{k} eq 'name' && $lhs->{v}!~/^@/ && !exists $env->{$lhs->{v}}){fail("assignment to undeclared local $lhs->{v}",$s->{file},1)}$must->($infer->($lhs,$env,$caps),$infer->($st->{rhs},$env,$caps),'assignment');}
   elsif($k eq 'require'||$k eq 'throw'){
    $must->('Bool',$infer->($st->{cond},$env,$caps),'require condition') if $k eq 'require';
    my $types=$exceptions{$st->{name}};for my $i(0..$#{$st->{args}}){$must->($types->[$i],$infer->($st->{args}[$i],$env,$caps),$st->{name})}$returns=1 if $k eq 'throw';
   }
   elsif($k eq 'grab'){my $table=$tables{substr($st->{table},1)};$must->($table->[0],$infer->($st->{key},$env,$caps),'grab key');my $t=$type->($st->{type});$must->($t,$table->[1],'grab type');$env->{$st->{var}}=$t;}
   elsif($k eq 'while'){$must->('Bool',$infer->($st->{cond},$env,$caps),'while condition');$block->($st->{body},{%$env},$caps,$ret)}
   elsif($k eq 'for'){my $t=$infer->($st->{items},$env,$caps);$t=~/^Vec<(.+)>$/ or fail('for requires a vector',$s->{file},1);$block->($st->{body},{%$env,$st->{var}=>$1},$caps,$ret)}
   elsif($k eq 'if'){$must->('Bool',$infer->($st->{cond},$env,$caps),'if condition');my $a=$block->($st->{yes},{%$env},$caps,$ret);my $b=$block->($st->{no},{%$env},$caps,$ret);$returns=1 if $a && $b;}
   elsif($k eq 'on'){$block->($st->{body},{%$env},$caps,$ret);}
  }
  return $returns;
 };
 for my $state(@{$s->{states}}){$must->($states{$state->{name}},$infer->($state->{initial_expr},{},{}),'state initializer')}
 $block->($s->{init_code},{},{},'Void');
 for my $f(@{$s->{functions}}){my $sig=$functions{$f->{name}};my %env=map {$_->[0]=>$_->[1]} @{$sig->{params}};
  my $returns=$block->($f->{code},\%env,{},$sig->{returns});
  $returns || $sig->{returns} eq 'Void' or fail("function $f->{name} may finish without returning $sig->{returns}",$s->{file},$f->{line});
 }
 my(%ruletypes,$active_facet);my $symtype;$symtype=sub{my($name)=@_;return $tokens{$name} if $tokens{$name};
  if(my $kind=$s->{internal}{$name}){
   my($p)=grep {$_->{lhs} eq $name && @{$_->{rhs}}>0 && $_->{rhs}[-1] ne $name} @{$s->{productions}};
   return 'Any' unless $p;
   my $inner=$symtype->($p->{rhs}[-1]);return "Vec<$inner>" if $kind =~ /^(\*|\+|sep)$/;return $inner;
  }
  return $ruletypes{$active_facet}{$name}||'Any';
 };
 for my $iteration(0..scalar @{$s->{rules}}){my $changed=0;
  for my $r(@{$s->{rules}}){for my $facet(sort keys %{$r->{code}}){$active_facet=$facet;my %caps;
   for my $p(grep {$_->{lhs} eq $r->{name}} @{$s->{productions}}){for my $i(0..$#{$p->{labels}}){
    my $label=$p->{labels}[$i];next unless $label;my $t=$symtype->($p->{rhs}[$i]);
    $must->($caps{$label},$t,"capture $label") if exists $caps{$label};$caps{$label}=$t;
   }}
   my @returns;$observed_returns=\@returns;$block->($r->{code}{$facet},{},\%caps,undef);$observed_returns=undef;
   my @known=grep {$_ ne 'Any'} @returns;my $t=@known?$known[0]:@returns?'Any':'Void';
   for my $got(@known){if($t eq 'Int' && $got eq 'Float64'){$t='Float64'}else{$must->($t,$got,"returns of rule $r->{name}")}}
   if(($ruletypes{$facet}{$r->{name}}||'') ne $t){$ruletypes{$facet}{$r->{name}}=$t;$changed=1;}
   $r->{return_types}{$facet}=$t;
  }}
  last unless $changed;
 }

 for my $r(@{$s->{rewrites_compiled}}){my %env=(fp_mode=>'String');
  for my $op(@{$r->{match}}){my @params=map {(split /\s*:\s*/,$_,2)[1]} comma_parts($ops{$op->{op}}{params});
   for my $i(0..$#{$op->{args}}){my $name=$op->{args}[$i];next unless $name=~/^[A-Za-z_]\w*$/;
    $must->($env{$name},$params[$i],'rewrite binding') if exists $env{$name};$env{$name}=$params[$i];
   }
  }
  $infer->($r->{when},\%env,{});
  for my $op(@{$r->{replace}}){my @params=map {(split /\s*:\s*/,$_,2)[1]} comma_parts($ops{$op->{op}}{params});
   for my $i(0..$#{$op->{args}}){$must->($params[$i],$infer->($op->{args}[$i],\%env,{}),'rewrite replacement')}
  }
 }
}

use MIME::Base64 qw(encode_base64);
sub generate_python {
 my($s,$o,$json)=@_;
 for my $flag(qw(header runtime semant rewriter table lexer parser_out ast ir driver)){
  defined $o->{$flag} and fail("Python output does not support --$flag",$s->{file},1);
 }
 $o->{split} =~ /^(none|explicit)$/ or fail('Python output supports split none or explicit',$s->{file},1);
 my $file=outpath($o->{output} || lc($s->{language}).'.py');
 my $encoded=encode_base64($json->encode($s),'');
 my $runtime=template('python');
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

sub cstr {my($s)=@_;$s =~ s/\\/\\\\/g;$s =~ s/"/\\"/g;$s =~ s/\n/\\n/g;$s =~ s/\r/\\r/g;$s =~ s/\t/\\t/g;return '"'.$s.'"'}
sub relative_include {
 my($file,$owner)=@_;
 return cstr(File::Spec->abs2rel(File::Spec->rel2abs($file),File::Spec->rel2abs(dirname($owner))));
}
sub write_outputs {
 my($files,$main)=@_;
 my %seen;
 for my $facet(qw(manifest depfile)){next unless defined $opt{$facet};my $path=outpath($opt{$facet});
  -d dirname($path) or die "output directory does not exist: ".dirname($path)."\n";
  my $abs=canonical_path($path);$seen{$abs}++ and die "output path collision: $path\n";
  (grep {$abs eq canonical_path($_)} ($input,__FILE__,map {$_->{path}} @{$spec->{dependencies}||[]})) and die "output would overwrite an input: $path\n";
 }
 for my $path(sort keys %$files){
  -d dirname($path) or die "output directory does not exist: ".dirname($path)."\n";
  my $abs=canonical_path($path);
  $seen{$abs}++ and die "output path collision: $path\n";
  (grep {$abs eq canonical_path($_)} ($input,__FILE__,map {$_->{path}} @{$spec->{dependencies}||[]})) and die "output would overwrite an input: $path\n";
 }
 for my $path(sort keys %$files){write_file($path,$files->{$path})}
 return ($main,grep {$_ ne $main} sort keys %$files);
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
 for my $facet(qw(header runtime semant rewriter table lexer parser_out ast ir driver json values memory numeric io api manifest depfile)){
  next unless defined $o->{$facet};
  next if $o->{split} eq 'none' && $facet !~ /^(header|manifest|depfile)$/;
  $paths{$facet}=outpath($o->{$facet});
 }
 my %seen;
 for my $facet(sort keys %paths){
  my $abs=canonical_path($paths{$facet});
  die "output path collision: $paths{$facet} ($facet and $seen{$abs})\n" if $seen{$abs};
  $seen{$abs}=$facet;
  $abs eq canonical_path($input) and die "output would overwrite specification: $paths{$facet}\n";
  if($facet !~ /^(header|table|manifest|depfile)$/){
   $paths{$facet} =~ /\Q$suffix\E$/ or die "incompatible $o->{T} output suffix: $paths{$facet}\n";
  }
 }
 my $constant="const char lls_spec_json[] = {\n";
 my @byte=unpack('C*',$json->encode($s));
 while(@byte){my @row=splice(@byte,0,32);$constant.='  '.join(', ',@row).",\n"}
 $constant.="  0\n};\n";
 my $code=template('c');
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
   $name =~ /^(compile_output|render_template|verify_ops|bytecode_id|encode_bytecode|execute_bytecode)$/?'ir':
   $name =~ /^(new_node|first_tok)$/?'ast':
   $name eq 'llsynth_generated_main'?'driver':'runtime';
  if($o->{split} eq 'all' && $facet eq 'runtime'){
   $facet=$name =~ /^(jws|jhex|jstring|jparse|jo|ji|js|jg|jlen|jnumber|jeq|lls_spec)$/?'json':
    $name =~ /^(ca|crealloc|clear_ctx|allocation_failure|dup_n)$/?'memory':
    $name =~ /^(ascii_strtod|ascii_format|number_literal|int_binary|quoted_string)$/?'numeric':
    $name =~ /^(build|read_source)$/?'io':
    $name =~ /^(api_|\Q${base}_\E)/?'api':'values';
  }
  $chunks{$facet}.=$body;
 }
 my %files;
 my $split=grep {exists $paths{$_}} qw(runtime semant rewriter lexer parser_out ir driver ast);
 my $private=File::Spec->catfile(dirname($main),$base.'_internal.h');
 die "output path collision: $private\n" if $split && $seen{canonical_path($private)};
 my $preamble=$header.$declarations;
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
 for my $path(keys %files){
  my $public=index($files{$path},$header)==0?substr($files{$path},0,length($header),''):'';
  $files{$path}=~s{(/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|\b[A-Za-z_]\w*\b)}{exists $renames{$1}?$renames{$1}:$1}gse;
  $files{$path}=$public.$files{$path};
 }
 return write_outputs(\%files,$main);
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

# All templates are stored in the Perl DATA section. This generator needs no
# sibling files, libraries from CPAN, or external programs at runtime.
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

@dataclass
class FunctionRef:
    name: str

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

class Context:
    def __init__(self):
        self.state = {}
        self.tables = {}
        self.node = None
        self.reset()
    def reset(self):
        self.node = None
        self.fuel = 1000000
        self.depth = 0
        self.state = {}
        for item in SPEC['states']:
            self.state[item['name']] = self.eval(item['initial_expr'], {}, {})
        self.tables = {item['name']: {} for item in SPEC['tables']}
        self.node = None
        self.exec_block(SPEC['init_code'], {}, {})
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
        if k == 'name':
            name = e['v']
            if name.startswith('@'): return self.state[name[1:]]
            if name.startswith('#'): return self.tables[name[1:]]
            if name.startswith('$'): return self.capture(name[1:], caps)
            if name.startswith('&'): return FunctionRef(name[1:])
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
                f = next((f for f in SPEC['functions'] if f['name'] == callee.name), None)
                if not f: raise LanguageError('SEM', f'undefined function {callee.name}')
                keys = [x.split(':')[0].strip() for x in f['args'].split(',') if x.strip()]
                if self.depth >= 128: raise LanguageError('RESOURCE', 'function recursion limit exceeded')
                self.depth += 1
                try: return self.exec_block(f['code'], dict(zip(keys, args)), {})[1]
                finally: self.depth -= 1
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
    def tick(self):
        self.fuel -= 1
        if self.fuel < 0: raise LanguageError('RESOURCE', 'action instruction budget exhausted')
    def exec_block(self, code, env, caps):
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
            elif k == 'let': env[stmt['name']] = self.eval(stmt['expr'], env, caps)
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
enum {VNIL,VNUM,VINT,VBOOL,VSTR,VVEC,VMAP,VFN,VOP,VBUILTIN};
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
 if(a.tag==VOP){if(strcmp(a.op->name,b.op->name))return 0;return eq_val(vvec(a.op->args),vvec(b.op->args));}
 return a.tag==b.tag&&strcmp(a.s?a.s:"",b.s?b.s:"")==0;}
static double number(Val v){return v.tag==VNUM?v.n:(v.tag==VINT||v.tag==VBOOL)?(double)v.i:0;}
static Val eval_call(Context *c,Val callee,Vec *args){Val nil=LLS_ZERO;const char *name=callee.s?callee.s:"";
 if(callee.tag==VOP){Op *p=(Op*)ca(c,sizeof(Op));p->name=callee.op->name;p->args=args;return vop(p);}
 if(callee.tag==VFN){Json *functions=jo(lls_spec(c),"functions");for(int i=0;i<jlen(functions);i++){Json *f=ji(functions,i);if(strcmp(jg(f,"name"),name))continue;
   Map *env=new_map(c),*caps=new_map(c);char *ar=dup_n(c,jg(f,"args"),strlen(jg(f,"args")));char *cur=ar;int x=0;
   while(*cur && x<args->n){char *end=strchr(cur,',');if(end)*end=0;char *colon=strchr(cur,':');if(colon)*colon=0;while(*cur==' ')cur++;map_put(c,env,dup_n(c,cur,strlen(cur)),args->a[x++]);if(!end)break;cur=end+1;}
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
