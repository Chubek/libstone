package main;
use strict;
use warnings;
use utf8;
use Digest::SHA qw(sha256_hex);
use Encode ();
use File::Basename qw(dirname);
use File::Spec ();
use Math::BigInt ();
use POSIX ();

# The internal frontend shares CLI options and helpers with the driver.
our (%opt, @include_paths);
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

1;
