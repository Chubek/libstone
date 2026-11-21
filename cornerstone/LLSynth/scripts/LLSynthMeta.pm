package main;
use strict;
use warnings;

# Compile-time expansion deliberately produces only the pre-existing action IR.
# No backend knows about templates, macros, substitution environments or marks.
sub meta_clone {
 my($v,$c)=@_;
 return [map {meta_clone($_,$c)} @$v] if ref($v) eq 'ARRAY';
 if(ref($v) eq 'HASH'){
  meta_tick($c) if $c;
  return {map {$_=>meta_clone($v->{$_},$c)} sort keys %$v};
 }
 return $v;
}

sub meta_error {
 my($c,$message)=@_;
 my $where=$c->{where};
 my $trace=join(' -> ',@{$c->{stack}});
 $message.=" (expanding $trace)" if length $trace;
 fail("metaprogramming: $message",$where->{file},$where->{line});
}

sub meta_tick {
 my($c)=@_;
 ++$c->{nodes}<=100000 or meta_error($c,'expansion exceeds 100000 AST visits');
}

sub meta_fresh {
 my($c)=@_;my $name;
 do {$name='__lls_meta_'.++$c->{serial}} while $c->{used}{$name};
 $c->{used}{$name}=1;
 return $name;
}

sub meta_context {
 my($s,$where)=@_;
 return {s=>$s,where=>$where,defs=>$s->{meta_defs},globals=>$s->{meta_globals},
         used=>{},serial=>0,nodes=>0,expansions=>0,stack=>[]};
}

sub meta_collect_names {
 my($c,$v)=@_;
 if(ref($v) eq 'HASH'){
  for my $key(sort keys %$v){
   my $value=$v->{$key};
   $c->{used}{$value}=1 if !ref($value) && defined($value);
   meta_collect_names($c,$value) if ref($value);
  }
 }elsif(ref($v) eq 'ARRAY'){meta_collect_names($c,$_) for @$v}
}

sub meta_enter {
 my($c,$d,$args)=@_;
 @{$args}==@{$d->{parameters}} or meta_error($c,"$d->{kind} '$d->{name}' expects ".scalar(@{$d->{parameters}})." arguments, got ".scalar(@$args)." (defined at $d->{file}:$d->{line})");
 grep {$_ eq $d->{name}} @{$c->{stack}} and meta_error($c,"recursive expansion of '$d->{name}'");
 @{$c->{stack}}<64 or meta_error($c,'expansion depth exceeds 64');
 ++$c->{expansions}<=4096 or meta_error($c,'expansion exceeds 4096 invocations');
 push @{$c->{stack}},$d->{name};
}

sub meta_expr {
 my($c,$e,$env,$definition)=@_;return undef unless $e;
 meta_tick($c);
 my $k=$e->{k};
 if($k eq 'name'){
  my $name=$e->{v};
  return meta_clone($env->{$name},$c) if exists $env->{$name};
  if($definition){
   $name !~ /^\$/ or meta_error($c,"free capture '$name' in definition; pass it as a parameter");
   exists($c->{defs}{$name}) and meta_error($c,"compile-time name '$name' must be called");
   $c->{globals}{$name} || $name =~ /^(?:true|false|nil|None)$/ or meta_error($c,"free name '$name' in definition; pass caller locals as parameters");
   # & references bypass caller environments in both native and Python runtimes.
   return {k=>'name',v=>'&'.$name} if $c->{globals}{$name} && $c->{globals}{$name} eq 'function';
  }
  return meta_clone($e,$c);
 }
 if($k eq 'call' && $e->{a}{k} eq 'name' && exists $c->{defs}{$e->{a}{v}}){
  my $d=$c->{defs}{$e->{a}{v}};
  $d->{kind} eq 'template' or meta_error($c,"macro '$d->{name}' requires a whole statement");
  my @args=map {meta_expr($c,$_,$env,$definition)} @{$e->{args}};
  meta_enter($c,$d,\@args);
  my %params;@params{@{$d->{parameters}}}=@args;
  my $result=meta_expr($c,$d->{ast},\%params,1);
  pop @{$c->{stack}};
  return $result;
 }
 if($k eq 'num'||$k eq 'str'){return meta_clone($e,$c)}
 my $result=meta_clone($e,$c);
 if($k eq 'bin'||$k eq 'index'){$result->{a}=meta_expr($c,$e->{a},$env,$definition);$result->{b}=meta_expr($c,$e->{b},$env,$definition)}
  elsif($k eq 'un'||$k eq 'await'){$result->{a}=meta_expr($c,$e->{a},$env,$definition)}
  elsif($k eq 'closure'){
   my %inner=%$env;my %seen;my @params;
   for my $param(@{$e->{params}}){
    push @params,{%$param,name=>meta_bind($c,$param->{name},\%inner,\%seen)};
   }
   $result->{params}=\@params;
   $result->{body}=meta_block($c,$e->{body},\%inner,$definition,\%seen);
  }
 elsif($k eq 'list'){$result->{args}=[map {meta_expr($c,$_,$env,$definition)} @{$e->{args}}]}
 elsif($k eq 'call'){
  $result->{a}=meta_expr($c,$e->{a},$env,$definition);
  $result->{args}=[map {meta_expr($c,$_,$env,$definition)} @{$e->{args}}];
 }else{meta_error($c,"unsupported expression '$k' during expansion")}
 return $result;
}

sub meta_bind {
 my($c,$name,$env,$seen)=@_;
 $name =~ /^(?:[A-Za-z_]\w*|t'[A-Za-z_]\w*)$/ or meta_error($c,"invalid local binder '$name'");
 exists($c->{defs}{$name}) and meta_error($c,"local binder '$name' conflicts with compile-time declaration");
 $seen->{$name}++ and meta_error($c,"duplicate local '$name'");
 my $fresh=meta_fresh($c);
 $env->{$name}={k=>'name',v=>$fresh};
 return $fresh;
}

sub meta_block {
 my($c,$code,$env,$definition,$initial_seen)=@_;
 my %seen=%{$initial_seen||{}};my @out;
 for my $st(@$code){
  meta_tick($c);my $k=$st->{k};
  if($k eq 'exec' && $st->{expr}{k} eq 'call' && $st->{expr}{a}{k} eq 'name'){
   my $d=$c->{defs}{$st->{expr}{a}{v}};
   if($d && $d->{kind} eq 'macro'){
    my @args=map {meta_expr($c,$_,$env,$definition)} @{$st->{expr}{args}};
    meta_enter($c,$d,\@args);
    my(%params,%bound);
    # Evaluate arguments before any body binders, exactly once, in source order.
    for my $i(0..$#args){
     my $name=meta_fresh($c);
     push @out,{k=>'let',name=>$name,type=>'',expr=>$args[$i]};
     $params{$d->{parameters}[$i]}={k=>'name',v=>$name};
     $bound{$d->{parameters}[$i]}=1;
    }
    push @out,@{meta_block($c,$d->{ast},\%params,1,\%bound)};
    pop @{$c->{stack}};
    next;
   }
  }
  my $new=meta_clone($st,$c);
  if($k eq 'let'){
   $new->{expr}=meta_expr($c,$st->{expr},$env,$definition);
   $new->{name}=meta_bind($c,$st->{name},$env,\%seen);
  }elsif($k eq 'grab'){
   if($definition && ($c->{globals}{$st->{table}}||'') ne 'table'){
    meta_error($c,"unknown table '$st->{table}' in definition");
   }
   $new->{key}=meta_expr($c,$st->{key},$env,$definition);
   $new->{var}=meta_bind($c,$st->{var},$env,\%seen);
  }elsif($k eq 'return'||$k eq 'exec'){$new->{expr}=meta_expr($c,$st->{expr},$env,$definition)}
  elsif($k eq 'assign'){
   $new->{lhs}=meta_expr($c,$st->{lhs},$env,$definition);
   $new->{rhs}=meta_expr($c,$st->{rhs},$env,$definition);
  }elsif($k eq 'throw'||$k eq 'require'){
   if($definition){
    my $name=$st->{name};$name='!'.$name unless $name =~ /^!/;
    ($c->{globals}{$name}||'') eq 'exception' or meta_error($c,"unknown exception '$name' in definition");
   }
   $new->{cond}=meta_expr($c,$st->{cond},$env,$definition) if $k eq 'require';
   $new->{args}=[map {meta_expr($c,$_,$env,$definition)} @{$st->{args}}];
  }elsif($k eq 'while'){
   $new->{cond}=meta_expr($c,$st->{cond},$env,$definition);
   $new->{body}=meta_block($c,$st->{body},{%$env},$definition);
  }elsif($k eq 'for'){
   $new->{items}=meta_expr($c,$st->{items},$env,$definition);
   my %inner=%$env;
   $new->{var}=meta_bind($c,$st->{var},\%inner,{});
   $new->{body}=meta_block($c,$st->{body},\%inner,$definition);
  }elsif($k eq 'if'){
   $new->{cond}=meta_expr($c,$st->{cond},$env,$definition);
   $new->{yes}=meta_block($c,$st->{yes},{%$env},$definition);
   $new->{no}=meta_block($c,$st->{no},{%$env},$definition);
  }elsif($k eq 'on'){
   $definition and meta_error($c,'on-branch capture in macro definition; pass captures as parameters');
   $new->{body}=meta_block($c,$st->{body},{%$env},0);
  }else{meta_error($c,"unsupported statement '$k' during expansion")}
  push @out,$new;
 }
 return \@out;
}

sub meta_prepare {
 my($s)=@_;
 my %globals=map {$_=> 'builtin'} qw(len pop top push emit print final_stack_height reverse min max abs is_finite spawn join sleep);
 $globals{$_->{name}}='function' for @{$s->{functions}};
 $globals{'&'.$_->{name}}='reference' for @{$s->{functions}};
 $globals{'@'.$_->{name}}='state' for @{$s->{states}};
 $globals{'#'.$_->{name}}='table' for @{$s->{tables}};
 $globals{'!'.$_->{name}}='exception' for @{$s->{exceptions}};
 $globals{$_}='operation' for keys %{$s->{op_by_name}};
 $globals{$_}='operation' for keys %{$s->{type_ops}};
 $s->{meta_globals}=\%globals;$s->{meta_defs}={};
 for my $d(@{$s->{metaprograms}}){
  my $c=meta_context($s,$d);
  exists($s->{meta_defs}{$d->{name}}) and meta_error($c,"duplicate compile-time declaration '$d->{name}'");
  $globals{$d->{name}} and meta_error($c,"compile-time declaration '$d->{name}' conflicts with runtime name");
  $d->{name} !~ /^__lls_/ or meta_error($c,"reserved name '$d->{name}'");
  my @params=length(strip_ws($d->{params}))?split(/,/,$d->{params},-1):();
  my %seen;
  for(@params){
   $_=strip_ws($_);
   /^[A-Za-z_]\w*$/ or meta_error($c,"invalid parameter '$_' in $d->{name}");
   $seen{$_}++ and meta_error($c,"duplicate parameter '$_' in $d->{name}");
  }
  $d->{parameters}=\@params;
  my $ast=eval {$d->{kind} eq 'template'?expr_snippet($d->{body}):parse_action($d->{body},$d->{file},$d->{line})};
  $@ and meta_error($c,"invalid $d->{kind} '$d->{name}': $@");
  $d->{ast}=$ast;$s->{meta_defs}{$d->{name}}=$d;
 }
 # Validate even unused definitions, including indirect recursion and bad arity.
 for my $d(@{$s->{metaprograms}}){
  my $c=meta_context($s,$d);
  meta_collect_names($c,$s);
  my %env=map {$_=>{k=>'name',v=>meta_fresh($c)}} @{$d->{parameters}};
  my %seen=map {$_=>1} @{$d->{parameters}};
  for my $param(@{$d->{parameters}}){
   exists($c->{defs}{$param}) and meta_error($c,"parameter '$param' conflicts with compile-time declaration");
  }
  meta_enter($c,$d,[map {$env{$_}} @{$d->{parameters}}]);
  if($d->{kind} eq 'template'){meta_expr($c,$d->{ast},\%env,1)}
  else{meta_block($c,$d->{ast},\%env,1,\%seen)}
 }
}

sub meta_compile_actions {
 my($s)=@_;return unless @{$s->{metaprograms}||[]};
 meta_prepare($s);
 my $c=meta_context($s,{file=>$s->{file},line=>1});
 meta_collect_names($c,$s);
  for my $f(@{$s->{functions}}){
   next if $f->{foreign};
  $c->{where}={file=>$s->{file},line=>$f->{line}};
  my(%env,%seen);my @args;
  for my $arg(comma_parts($f->{args})){
   my($name,$type)=split /\s*:\s*/,$arg,2;
   my $fresh=meta_bind($c,strip_ws($name),\%env,\%seen);
   push @args,$fresh.(defined($type)?': '.$type:'');
  }
  $f->{args}=join(', ',@args);
  $f->{parameters}=[map {my($name,$type)=split /\s*:\s*/,$_,2;{name=>$name,type=>$type}} @args];
  $f->{code}=meta_block($c,$f->{code},\%env,0,\%seen);
 }
 $c->{where}={file=>$s->{file},line=>1};
 $s->{init_code}=meta_block($c,$s->{init_code},{},0);
 for my $r(@{$s->{rules}}){
  $c->{where}={file=>$s->{file},line=>$r->{line}};
  for my $key(qw(semantics lower analyze emit)){
   $r->{code}{$key}=meta_block($c,$r->{code}{$key},{},0) if exists $r->{code}{$key};
  }
 }
}

sub meta_compile_passes {
 my($s)=@_;return unless @{$s->{metaprograms}||[]};
 my $c=meta_context($s,{file=>$s->{file},line=>1});
 meta_collect_names($c,$s);
 for my $state(@{$s->{states}}){$state->{initial_expr}=meta_expr($c,$state->{initial_expr},{},0)}
 for my $r(@{$s->{rewrites_compiled}}){
  $r->{when}=meta_expr($c,$r->{when},{},0);
  for my $op(@{$r->{replace}}){$op->{args}=[map {meta_expr($c,$_,{},0)} @{$op->{args}}]}
 }
 # No compile-time ASTs or expander state are needed by generated runtimes.
 delete @{$s}{qw(meta_defs meta_globals metaprograms)};
}

1;
