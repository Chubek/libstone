package main;
use strict;
use warnings;
use File::Basename qw(dirname);
use File::Spec ();
use Digest::SHA qw(sha256_hex);

# Specification modules are linked before the ordinary frontend runs. Neither
# loading nor linking evaluates Perl, Python, C, or other host-language source.
our @module_paths;

sub module_prefix {
 my($unit)=@_;
 return 'LLSMod_'.substr(sha256_hex($unit->{root}?'entry:':'module:'.$unit->{name}),0,24).'_';
}

sub module_find {
 my($name,$named,$file,$line)=@_;
 my $relative=$named ? join('/',split /\./,$name).'.llsynth' : $name;
 my @tried;
 for my $root(dirname($file),@module_paths){
  my $path=File::Spec->file_name_is_absolute($relative)?$relative:File::Spec->catfile($root,$relative);
  push @tried,$path;
  return canonical_path($path) if -f $path;
 }
 fail("module '$name' not found (searched: ".join(', ',@tried).")",$file,$line);
}

sub module_header {
 my($line)=@_;
 return ('grammar',$1) if $line =~ /^\.(?:rule|token|literal)\s+(\w+)/;
 return ('function',$1) if $line =~ /^\.(?:function|async\s+function|extern\s+c)\s+(\w+)\s*\(/;
 return ('function',$1) if $line =~ /^\.sandstone\s+(\w+)\s+bank\b/;
 return ('@',$1) if $line =~ /^\.state\s+\@(\w+)/;
 return ('#',$1) if $line =~ /^\.symtbl\s+\#(\w+)/;
 return ('!',$1) if $line =~ /^\.exception\s+\!(\w+)/;
 return ('%',$1) if $line =~ /^\.type\s+%(\w+)/;
 return ('ir',$1) if $line =~ /^\.ir\s+(\w+)/;
 return ('meta',$1) if $line =~ /^\.(?:macro|template)\s+(\w+)/;
 return ();
}

sub module_block {
 my($line)=@_;
 return 'rule' if $line =~ /^\.rule\b/;
 return 'function' if $line =~ /^\.(?:function|async\s+function)\b/;
 return $1 if $line =~ /^\.(init|ir|rewrite|backend|analysis|pipeline|macro)\b/;
 return '';
}

sub read_module_spec_lines {
 my($file,$deps)=@_;
 my(%units,%names,@stack,@out);
 my $load;
 $load=sub {
  my($path,$import_file,$import_line,$expected)=@_;
  my $id=canonical_path($path);
  if(my $old=$units{$id}){
   if($old->{active}){
    fail('module import cycle: '.join(' -> ',map {$_->{file}} @stack,$old),
         $import_file||$path,$import_line||1);
   }
   if(defined $expected && $old->{name} ne $expected){
    fail("module name mismatch: expected '$expected', found '$old->{name}'",$import_file,$import_line);
   }
   return $old;
  }
  my $unit={file=>$path,root=>!defined($import_file),active=>1,imports=>{},decls=>{},exports=>{},lines=>[]};
  $units{$id}=$unit;push @stack,$unit;
  my @lines=read_spec_lines($path,{},$deps);
  my($block,$seen)=('','');
  for my $entry(@lines){
   my($raw,$origin,$ln)=@$entry;my $l=strip_ws($raw);
   if(!$block && $l =~ /^\.module\b/){
    $l =~ /^\.module\s+([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)\s*$/
      or fail('expected .module Name',$origin,$ln);
    my $name=$1;
    !$seen && !$unit->{name} or fail('.module must be the first directive',$origin,$ln);
    canonical_path($origin) eq $id or fail('use .import, not .include, for a .module file',$origin,$ln);
    exists($names{$name}) && $names{$name} ne $id
      and fail("module '$name' is declared by both $names{$name} and $id",$origin,$ln);
    $unit->{name}=$name;$names{$name}=$id;
    next;
   }
   next if !$block && ($l eq '' || $l =~ /^#(?:\s|$)/);
   $seen=1;
   if(!$block && $l =~ /^\.import\b/){
    $l =~ /^\.import\s+(?:"([^"\n]+)"|([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*))(?:\s+as\s+([A-Za-z_]\w*))?\s*$/
      or fail('expected .import "file.llsynth" [as Alias] or .import Name [as Alias]',$origin,$ln);
    my($quoted,$named,$alias)=($1,$2,$3);
    my $found=module_find(defined($quoted)?$quoted:$named,defined($named),$origin,$ln);
    my $child=$load->($found,$origin,$ln,$named);
    $alias//=($child->{name}=~/([^.]+)$/)[0];
    $alias !~ /__/ or fail('module aliases cannot contain __',$origin,$ln);
    if(my $old=$unit->{imports}{$alias}){
     $old == $child or fail("module alias '$alias' is already bound to $old->{name}",$origin,$ln);
    }
    $unit->{imports}{$alias}=$child;
    next;
   }
   if(!$block && $l =~ /^\.export\b/){
    $l =~ /^\.export\s+(.+)$/ or fail('expected .export Name, otherName',$origin,$ln);
    my @exports=split /\s*,\s*/,$1,-1;
    for my $name(@exports){
     $name =~ /^[\@#!%]?[A-Za-z_]\w*$/ or fail("invalid export '$name'",$origin,$ln);
     exists $unit->{exports}{$name} and fail("duplicate export '$name'",$origin,$ln);
     $unit->{exports}{$name}=[$origin,$ln];
    }
    next;
   }
   if(!$unit->{root} && !$block && $l =~ /^\.(language|version|summary|start|option|pipeline)\b/){
    fail(".$1 is entry-specification configuration, not a module declaration",$origin,$ln);
   }
   if(!$block){
    my($kind,$name)=module_header($l);
    if(defined $kind){
     my $key="$kind:$name";
     exists($unit->{decls}{$key}) && !$unit->{root}
       and fail("duplicate module declaration '$name'",$origin,$ln);
     $name =~ /^LLSMod_/ and fail("reserved module linker name '$name'",$origin,$ln);
     $unit->{decls}{$key}={name=>$name,kind=>$kind,file=>$origin,line=>$ln};
    }
    $block=module_block($l);
   }elsif(($block eq 'rule' && $l eq '..') || ($block ne 'rule' && $l eq '.end')){
    $block='';
   }
   my $tagged=[$raw,$origin,$ln,$unit];
   push @{$unit->{lines}},$tagged;push @out,$tagged;
  }
  if(!$unit->{root} && !$unit->{name}){
   fail("imported specification '$path' requires .module Name",$import_file,$import_line);
  }
  if(defined $expected && ($unit->{name}||'') ne $expected){
   fail("module name mismatch: expected '$expected', found '".($unit->{name}||'')."'",$import_file,$import_line);
  }
  if(!$unit->{name} && keys %{$unit->{exports}}){
   my $where=$unit->{exports}{(sort keys %{$unit->{exports}})[0]};
   fail('.export requires .module Name',@$where);
  }
  for my $d(values %{$unit->{decls}}){
   # Hide entry-file functions and type namespaces too: an unbound bare name
   # or type in a module must not accidentally bind to an entry-file symbol.
   my $isolate=!$unit->{root} || ($d->{kind}=~/^(?:function|meta|%|ir)$/ && keys %{$unit->{imports}});
   $d->{linked}=$isolate?module_prefix($unit).$d->{name}:$d->{name};
   my $export=($d->{kind}=~/^[\@#!%]$/?$d->{kind}:'').$d->{name};
   $d->{public}=exists $unit->{exports}{$export};
  }
  for my $export(sort keys %{$unit->{exports}}){
   my $found=grep {my $key=($_->{kind}=~/^[\@#!%]$/?$_->{kind}:'').$_->{name};$key eq $export} values %{$unit->{decls}};
   $found or fail("export '$export' does not name a local declaration",@{$unit->{exports}{$export}});
  }
  $unit->{active}=0;pop @stack;
  return $unit;
 };
  my $root=$load->($file);
 # Preserve the old frontend byte-for-byte when there are no module directives.
 if(keys(%units)==1 && !$root->{name} && !keys %{$root->{imports}}){
  return map {[@$_[0..2]]} @out;
 }
 # A default entry rule must belong to the entry specification, not whichever
 # imported module happened to be visited first.
 my($start)=grep {$_->[0] =~ /^\s*\.start\b/ && $_->[3] == $root} @out;
 if(!$start){
  my($rule)=grep {$_->[0] =~ /^\s*\.rule\s+(\w+)\s*$/ && $_->[3] == $root} @out;
  if($rule){$rule->[0]=~/^\s*\.rule\s+(\w+)/;push @out,[".start $1",$rule->[1],$rule->[2],$root]}
 }
 for my $unit(sort {$a->{file} cmp $b->{file}} values %units){
  # An import alias reserves Alias__... in the importing scope.
  for my $d(sort {$a->{name} cmp $b->{name}} values %{$unit->{decls}}){
   for my $alias(sort keys %{$unit->{imports}}){
    index($d->{name},$alias.'__')==0
      and fail("declaration '$d->{name}' conflicts with module alias '$alias'",$d->{file},$d->{line});
   }
  }
  module_rewrite_unit($unit);
 }
 return map {[@$_[0..2]]} @out;
}

# Tokens retain their original spelling. Strings and comments are opaque; this
# is symbol linking, not substitution inside regexes, templates, or messages.
sub module_lex {
 my($text,$state)=@_;my @tokens;my $local_state='';$state||=\$local_state;
 while(length $text){
  if($$state || $text =~ /^["']/){
   my $quote=$$state||substr($text,0,1);
   my $start=$$state?0:1;my($end,$escape,$closed)=($start,0,0);
   for(;$end<length($text);$end++){
    my $char=substr($text,$end,1);
    if($escape){$escape=0;next}
    if($char eq '\\'){$escape=1;next}
    if($char eq $quote){$end++;$closed=1;last}
   }
   $$state=$closed?'':$quote;
   push @tokens,{raw=>substr($text,0,$end,'')};next;
  }
  if($text =~ s/^(\/\/[^\n]*|\#\s+[^\n]*)//s){
   push @tokens,{raw=>$1};next;
  }
  if($text =~ s/^(t'[A-Za-z_]\w*|[\@#\$!%&]?[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)?)//){
   push @tokens,{raw=>$1,name=>$1};next;
  }
  $text =~ s/^(.)//s;push @tokens,{raw=>$1};
 }
 return \@tokens;
}

sub module_resolve {
 my($unit,$kind,$name,$file,$line)=@_;
 $name =~ /^LLSMod_/ and fail("reserved module linker name '$name'",$file,$line);
 my @kinds=$kind eq 'callable'?('function','meta'):($kind);
 for my $category(@kinds){
  if(my $d=$unit->{decls}{"$category:$name"}){return $d->{linked}}
 }
 for my $alias(sort keys %{$unit->{imports}}){
  next unless index($name,$alias.'__')==0;
  my $short=substr($name,length($alias)+2);my $target=$unit->{imports}{$alias};
  for my $category(@kinds){
   my $d=$target->{decls}{"$category:$short"};
   return $d->{linked} if $d && $d->{public};
  }
  fail("module '$target->{name}' does not export ".($kind=~/^[\@#!%]$/?$kind:'').$short,$file,$line);
 }
 return undef;
}

sub module_rewrite_text {
 my($text,$unit,$mode,$file,$line)=@_;
 my $tokens=module_lex($text,\$unit->{$mode.'_quote'});
 for my $i(0..$#$tokens){
  my $t=$tokens->[$i];next unless defined $t->{name};
  my $name=$t->{name};
  $name =~ /^(?:[\@#\$!%&])?LLSMod_/ and fail("reserved module linker name '$name'",$file,$line);
  next if $name =~ /^(?:\$|t')/;
  if($mode eq 'action'){
   my $previous=$i-1;$previous-- while $previous>=0 && $tokens->[$previous]{raw}=~/^\s+$/;
   next if $previous>=0 && $tokens->[$previous]{raw} eq 'on'; # branch capture label
  }
  my($sigil,$bare)=$name =~ /^([\@#!%&]?)(.*)$/;
  my($kind,$suffix)=('','');
  if($mode eq 'grammar'){
   my $next=join('',map {$_->{raw}} @$tokens[$i+1..$#$tokens]);
   next if $next =~ /^\s*:/; # capture labels are not declarations
   $kind='grammar';
  }elsif($sigil ne ''){$kind=$sigil eq '&'?'function':$sigil}
  elsif($bare =~ /^(\w+)(\.\w+)$/){($bare,$suffix)=($1,$2);$kind='ir'}
  elsif($mode eq 'meta'){$kind='meta'}
  else{$kind='callable'}
  my $resolved=module_resolve($unit,$kind,$bare,$file,$line);
  if(defined $resolved){$t->{raw}=$sigil.$resolved.$suffix}
  elsif(!$unit->{root} && ($kind eq 'grammar' && $bare ne 'EOF' || $sigil ne '' || $kind eq 'ir')){
   # An unresolved module reference must never bind to an entry-file symbol.
   fail("undeclared ".($kind eq 'grammar'?'grammar symbol':$kind eq 'ir'?'IR namespace':"symbol $sigil")." '$bare' in module '$unit->{name}'",$file,$line);
  }
 }
 return join('',map {$_->{raw}} @$tokens);
}

sub module_action_locals {
 my($text,$state)=@_;my $tokens=module_lex($text,$state);
 my @t=grep {defined($_->{name}) || $_->{raw} !~ /^\s+$/} @$tokens;my %locals;
 for my $i(0..$#t){
  my $name=$t[$i]{name}//'';
  if($name eq 'let' || $name eq 'for'){
   $locals{$t[$i+1]{name}//''}=1 if $i<$#t;
  }
   if($name =~ /^[A-Za-z_]\w*$/ && $i<$#t && $t[$i+1]{raw} eq '=' && ($i+2>$#t || $t[$i+2]{raw} ne '=')){
    $locals{$name}=1;
   }
   if($name =~ /^[A-Za-z_]\w*$/ && $i<$#t && $t[$i+1]{raw} eq ':'){
    $locals{$name}=1; # typed closure parameters and local declarations
   }
  if($name eq 'into' && $i+3<=$#t && $t[$i+2]{raw} eq '('){
   $locals{$t[$i+3]{name}//''}=1;
  }
 }
 return %locals;
}

sub module_rewrite_types {
 my($text,$unit,$file,$line)=@_;
 my $tokens=module_lex($text);
 for my $t(@$tokens){
  next unless defined $t->{name};
  if($t->{name} =~ /^%(\w+)$/){
   $t->{raw}='%'.(module_resolve($unit,'%',$1,$file,$line)//$1);
  }elsif($t->{name} =~ /^(\w+)(\.Op)$/){
   $t->{raw}=(module_resolve($unit,'ir',$1,$file,$line)//$1).$2;
  }
 }
 return join('',map {$_->{raw}} @$tokens);
}

sub module_rewrite_unit {
 my($unit)=@_;
 # Callable declaration names are reserved within module action scopes. This
 # avoids confusing a local variable with a bare function reference.
 my %locals;
 for my $entry(@{$unit->{lines}}){
  my $l=strip_ws($entry->[0]);
  if($l =~ /^\.(?:function|async\s+function)\s+\w+\s*\((.*?)\)/){
   for my $arg(split /,/,$1){$locals{$1}=1 if $arg =~ /^\s*(\w+)\s*:/}
  }elsif($l =~ /^\.(?:macro|template)\s+\w+\s*\((.*?)\)/){
   for my $arg(split /,/,$1){$locals{$1}=1 if $arg =~ /^\s*(\w+)\s*$/}
  }
 }
 my($block,$facet)=('','');
 my %action_locals;my $quote='';
 for my $entry(@{$unit->{lines}}){
  my $l=strip_ws($entry->[0]);
  if(!$block){$block=module_block($l);next}
  if(($block eq 'rule' && $l eq '..') || ($block ne 'rule' && $l eq '.end')){$block='';$facet='';next}
  if($block eq 'rule' && $l =~ /^'(syntax|semantics|lower|analyze|emit)\s*:/){$facet=$1}
  if($block eq 'function'||$block eq 'init'||$block eq 'macro'||($block eq 'rule' && $facet ne 'syntax')){
   (my $action=$entry->[0]) =~ s/^\s*'\w+\s*:\s*//;
   %action_locals=(%action_locals,module_action_locals($action,\$quote));
  }
 }
 %locals=(%locals,%action_locals);$unit->{locals}=\%locals;
 for my $name(sort keys %locals){
  if(exists($unit->{decls}{"function:$name"}) && $unit->{decls}{"function:$name"}{linked} ne $name){
   fail("module local '$name' shadows a function declaration; use a distinct local name",$unit->{file},1);
  }
  if(exists($unit->{decls}{"meta:$name"}) && $unit->{decls}{"meta:$name"}{linked} ne $name){
   fail("module local '$name' shadows a metaprogram declaration; use a distinct local name",$unit->{file},1);
  }
  for my $alias(sort keys %{$unit->{imports}}){
   index($name,$alias.'__')==0 and fail("local '$name' conflicts with module alias '$alias'",$unit->{file},1);
  }
 }
 ($block,$facet)=('','');
 for my $entry(@{$unit->{lines}}){
  my($raw,$file,$line)=@$entry;my $l=strip_ws($raw);
  if(!$block){
   my($kind,$name)=module_header($l);
   if(defined $kind){
    my $d=$unit->{decls}{"$kind:$name"};
    if($kind eq '@'){
     if($raw =~ /^(.+?=\s*)(.*)$/){
      my($header,$value)=($1,$2);
      $raw=module_rewrite_types($header,$unit,$file,$line).module_rewrite_text($value,$unit,'action',$file,$line);
     }
    }elsif($kind eq 'function' || $kind eq '%' || $kind eq '#' || $kind eq '!'){
     # Only type spellings are linked here, not function argument identifiers.
     $raw=module_rewrite_types($raw,$unit,$file,$line);
    }elsif($kind eq 'meta' && $l =~ /^\.template\b/ && $raw =~ /^(.+?=\s*)(.*)$/){
     my($header,$expression)=($1,$2);
     $raw=$header.module_rewrite_text($expression,$unit,'action',$file,$line);
    }elsif($kind eq 'grammar' && $l =~ /^\.token\b/ && $raw =~ /^(.*?)(\s*=\s*\/.*)$/){
     my($header,$regex)=($1,$2);
     $raw=module_rewrite_types($header,$unit,$file,$line).$regex;
    }
    # Only the declared name, not argument names, regexes, or literal payloads.
    # Types are processed first so source cannot refer to private linker names.
    $raw =~ s/^(\s*\.(?:async\s+function|extern\s+c|\w+)\s+[\@#!%]?)\Q$name\E\b/$1$d->{linked}/;
   }elsif($l =~ /^\.start\s+(?:"(\w+)"|(\w+))\s*$/){
    my $name=$1//$2;
    $raw='.start '.(module_resolve($unit,'grammar',$name,$file,$line)//$name);
   }elsif($l =~ /^\.precedence\s+(left|right|nonassoc)\s+(.+)$/){
    $raw=".precedence $1 ".module_rewrite_text($2,$unit,'grammar',$file,$line);
   }elsif($l =~ /^\.rewrite\s+(\w+)\s*:\s*(\w+)\s*$/){
    my($name,$ir)=($1,$2);
    $name=module_prefix($unit).$name unless $unit->{root};
    $raw=".rewrite $name: ".(module_resolve($unit,'ir',$ir,$file,$line)//$ir);
   }elsif($l =~ /^\.backend\s+(\w+)\s+for\s+(\w+)\s*$/){
    $raw=".backend $1 for ".(module_resolve($unit,'ir',$2,$file,$line)//$2);
   }
   $block=module_block($l);
  }elsif(($block eq 'rule' && $l eq '..') || ($block ne 'rule' && $l eq '.end')){
   $block='';$facet='';
  }elsif($block eq 'rule'){
   if($l =~ /^'(syntax|semantics|lower|analyze|emit)\s*:\s*(.*)$/){
    $facet=$1;$raw="'$facet: ".module_rewrite_text($2,$unit,$facet eq 'syntax'?'grammar':'action',$file,$line);
   }elsif($l eq "'end"){$facet=''}
   elsif($facet){$raw=module_rewrite_text($raw,$unit,$facet eq 'syntax'?'grammar':'action',$file,$line)}
   }elsif($block eq 'function' || $block eq 'init' || $block eq 'macro'){
   $raw=module_rewrite_text($raw,$unit,'action',$file,$line);
  }elsif($block eq 'ir'){
   $raw=module_rewrite_types($raw,$unit,$file,$line);
  }elsif($block eq 'rewrite' && $l =~ /^\.when\s+(.+)$/){
   $raw='.when '.module_rewrite_text($1,$unit,'action',$file,$line);
  }
  $entry->[0]=$raw;
 }
}
1;
