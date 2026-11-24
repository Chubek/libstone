package main;
use strict;
use warnings;
use File::Basename qw(dirname);
use File::Spec ();
use Digest::SHA qw(sha256_hex);

# Specification modules are linked before the ordinary frontend runs.
our @module_paths;
my $module_name=qr/[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*/;
my $module_arrow=qr/${module_name}->$module_name/;
my $module_ref=qr/(?:$module_arrow|$module_name)/;
my $stdlib_dir=File::Spec->catdir(dirname(__FILE__),'..','stdlib');

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
 if($named && $name =~ /^std\.(.+)$/){
  my $path=File::Spec->catfile($stdlib_dir,split(/\./,$1));
  $path.='.llsynth';push @tried,$path;
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
 return ('%',$1) if $line =~ /^\.(?:type|sumtype|prodtype)\s+%(\w+)/;
 return ('ir',$1) if $line =~ /^\.ir\s+(\w+)/;
 return ('meta',$1) if $line =~ /^\.(?:macro|template)\s+(\w+)/;
 return ();
}

sub module_block {
 my($line)=@_;
 return 'rule' if $line =~ /^\.rule\b/;
 return 'function' if $line =~ /^\.(?:function|async\s+function)\b/;
 return $1 if $line =~ /^\.(init|ir|rewrite|backend|analysis|pipeline|macro|sumtype|prodtype)\b/;
 return '';
}

sub read_module_spec_lines {
 my($file,$deps,$receivers)=@_;
 $receivers//={};
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
   my($block,$seen,$member_owner)=('','',undef);
  for(my $i=0;$i<@lines;$i++){
   my $entry=$lines[$i];
   my($raw,$origin,$ln)=@$entry;my $l=strip_ws($raw);
   if($block eq 'sumtype' || $block eq 'prodtype'){
    $raw =~ s{\s*//.*$}{};$l=strip_ws($raw);
   }
   if(!$block && $l =~ /^\.(module|library)\b/){
    my $kind=$1;
    $l =~ /^\.\Q$kind\E\s+(?:"($module_name)"|($module_name))$/
      or fail("expected .$kind Name",$origin,$ln);
    my $name=$1//$2;
    !$seen && !$unit->{name} or fail(".$kind must be the first directive",$origin,$ln);
    canonical_path($origin) eq $id or fail("use .import, not .include, for a .$kind file",$origin,$ln);
    exists($names{$name}) && $names{$name} ne $id
      and fail("module '$name' is declared by both $names{$name} and $id",$origin,$ln);
    $unit->{kind}=$kind;$unit->{name}=$name;$names{$name}=$id;
    next;
   }
   next if !$block && ($l eq '' || $l =~ /^#(?:\s|$)/ || $l =~ m{^//});
   $seen=1;
   if(!$block && $l =~ /^\.extern\s+(?:(perl|c)\s+)?"([^"\n]+)"$/){
    my($flavor,$name)=($1,$2);
    my @generated=LLSynthCompanions::declarations($flavor,$name,$origin,$ln,$deps,$unit->{companions}//={},$unit->{own_blocks}//={});
    LLSynthCompanions::bind_blocks($unit->{blocks}//={},$unit->{own_blocks},$origin,$ln);
    splice @lines,$i+1,0,@generated;
    next;
   }
   if(!$block && $l =~ /^\.alias\b/){
    module_read_alias($unit,$l,$origin,$ln);next;
   }
   if(!$block && $l =~ /^\.import\b/){
    $l =~ /^\.import\s+(?:(qualified)\s+)?(?:"([^"\n]+)"|($module_name))(?:(?:\s+as\s+([A-Za-z_]\w*))|(?:\s+(qualified)))?$/
      or fail('expected .import [qualified] Name or "file.llsynth" [as Alias]',$origin,$ln);
    my($qualified,$quoted,$named,$alias,$post)=($1,$2,$3,$4,$5);
    $qualified && $post and fail("duplicate 'qualified'",$origin,$ln);
    $qualified ||= $post;
    # Quoted dotted names are the normal library spelling. Keep explicit
    # filesystem paths (including .llsynth filenames) literal.
    if(defined($quoted) && $quoted =~ /^$module_name$/ && $quoted !~ /\.llsynth$/ &&
       !grep {-f File::Spec->catfile($_,$quoted)} (dirname($origin),@module_paths)){
     ($named,$quoted)=($quoted,undef);
    }
    my $found=module_find(defined($quoted)?$quoted:$named,defined($named),$origin,$ln);
    my $child=$load->($found,$origin,$ln,$named);
    LLSynthCompanions::bind_blocks($unit->{blocks}//={},$child->{own_blocks},$origin,$ln) if $child->{own_blocks};
    if(!$alias && !$qualified && $child->{kind} eq 'library'){
     # One binding per exported declaration in this scope; a second
     # import of the same file is idempotent.
     for my $key(sort keys %{$child->{decls}}){
      my $d=$child->{decls}{$key};next unless $d->{public};
      my $old=$unit->{opened}{$key};
      $old && $old != $d and fail("ambiguous import '$d->{name}'; use qualified or as",$origin,$ln);
      $unit->{opened}{$key}=$d;
     }
    }
    defined($alias) && $alias =~ /__/ and fail('module aliases cannot contain __',$origin,$ln);
    $alias//= $qualified ? $child->{name} : ($child->{name}=~/([^.]+)$/)[0];
    $alias =~ s/\./__/g;
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
   if(!$block && ($unit->{kind}//'') eq 'library' && $l =~ /^\.summary\b/){
    $l =~ /^\.summary\s+"((?:\\.|[^"\\])*)"$/
      or fail('expected .summary "text"',$origin,$ln);
    defined $unit->{summary} and fail('duplicate .summary',$origin,$ln);
    $unit->{summary}=$1;
    next;
   }
   if(!$unit->{root} && !$block && $l =~ /^\.(language|version|summary|start|option|pipeline)\b/){
    fail(".$1 is entry-specification configuration, not a module declaration",$origin,$ln);
   }
   if(!$block){
    if($l =~ /^\.(\w+)\b/ && (my $provider=$unit->{blocks}{$1})){
     my @generated=LLSynthCompanions::expand_block($provider,\@lines,\$i,$deps);
     splice @lines,$i+1,0,@generated;
     next;
    }
    my($kind,$name)=module_header($l);
    if(defined $kind){
     my $key="$kind:$name";
     exists($unit->{decls}{$key}) && !$unit->{root}
       and fail("duplicate module declaration '$name'",$origin,$ln);
      $name =~ /^LLSMod_/ and fail("reserved module linker name '$name'",$origin,$ln);
      $kind eq '%' && builtin_sigil_type($name) and fail("reserved builtin type '%$name'",$origin,$ln);
      $unit->{decls}{$key}={name=>$name,kind=>$kind,file=>$origin,line=>$ln};
      $member_owner=$unit->{decls}{$key};
      $member_owner->{constructor}=1 if $l =~ /^\.prodtype\b/;
      $member_owner->{type_base}=$1 if $l =~ /^\.type\s+%\w+\s*=\s*%($module_ref)$/;
    }
    $block=module_block($l);
    }elsif(ends_spec_block($block,$l)){
     $block='';$member_owner=undef;
    }elsif($block eq 'ir' && $l =~ /^\.op\s+(\w+)/){
     $member_owner->{members}{$1}=1;
    }elsif(($block eq 'sumtype' || $block eq 'prodtype') && $l =~ /^(\w+)\s*:/){
     my $member=$1;$member_owner->{members}{$member}=1;
     if($block eq 'sumtype'){$member_owner->{members}{'is_'.$member}=$member_owner->{members}{'get_'.$member}=1}
   }
   my $tagged=[$raw,$origin,$ln,$unit];
   push @{$unit->{lines}},$tagged;push @out,$tagged;
  }
  if(!$unit->{root} && !$unit->{name}){
   fail("imported specification '$path' requires .module Name or .library Name",$import_file,$import_line);
  }
  if(defined $expected && ($unit->{name}||'') ne $expected){
   fail("module name mismatch: expected '$expected', found '".($unit->{name}||'')."'",$import_file,$import_line);
  }
  if(!$unit->{name} && keys %{$unit->{exports}}){
   my $where=$unit->{exports}{(sort keys %{$unit->{exports}})[0]};
   fail('.export requires .module Name',@$where);
  }
  for my $d(values %{$unit->{decls}}){
   exists($unit->{opened}{"$d->{kind}:$d->{name}"})
     and fail("declaration '$d->{name}' conflicts with a plain import; use qualified or as",$d->{file},$d->{line});
   # Hide entry-file functions and type namespaces too: an unbound bare name
   # or type in a module must not accidentally bind to an entry-file symbol.
   my $isolate=!$unit->{root} || ($d->{kind}=~/^(?:function|meta|%|ir)$/ && keys %{$unit->{imports}});
   $d->{linked}=$isolate?module_prefix($unit).$d->{name}:$d->{name};
   my $export=($d->{kind}=~/^[\@#!%]$/?$d->{kind}:'').$d->{name};
   $d->{public}=exists($unit->{exports}{$export}) ||
                (($unit->{kind}//'') eq 'library' && !keys %{$unit->{exports}});
  }
   module_bind_aliases($unit);
   for my $export(sort keys %{$unit->{exports}}){
   my $found=grep {my $key=($_->{kind}=~/^[\@#!%]$/?$_->{kind}:'').$_->{name};$key eq $export} values %{$unit->{decls}};
   $found or fail("export '$export' does not name a local declaration",@{$unit->{exports}{$export}});
  }
  $unit->{active}=0;pop @stack;
  return $unit;
 };
  my $root=$load->($file);
 # Preserve the old frontend byte-for-byte when there are no module directives.
  if(keys(%units)==1 && !$root->{name} && !keys %{$root->{imports}} && !keys %{$root->{aliases}||{}} &&
     !grep {index($_->[0],'LLSMod_colon_')>=0} @out){
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
  local $unit->{receivers}=$receivers;
  module_rewrite_unit($unit);
 }
 return map {[@$_[0..2]]} @out;
}

# Tokens retain their original spelling. Strings and comments are opaque; this
# is symbol linking, not substitution inside regexes, templates, or messages.
sub module_lex {
 my($text,$state,$mode,$unit)=@_;my @tokens;my $local_state='';$state||=\$local_state;
 $mode//='';
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
  if($mode ne '' && $text =~ /^([\@#!%&]?)($module_name)->($module_name)/){
   my($sigil,$alias,$member)=($1,$2,$3);
   # A type arrow can also be an ordinary separator, e.g. %Local->Int
   # inside a callable type. Only bound aliases qualify a type.
   my $is_qualified=$mode eq 'grammar' ||
                    ($mode eq 'type' || $sigil eq '%' ? module_import_target($unit,$alias)
                     : $sigil ne '' || $member =~ /\./ && module_import_target($unit,$alias));
   if($is_qualified){
    my $reference=$sigil.$alias.'->'.$member;
    substr($text,0,length($reference),'');
    push @tokens,{raw=>$reference,name=>$reference};next;
   }
  }
  if($text =~ s/^(t'[A-Za-z_]\w*|[\@#\$!%&]?[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)//){
   push @tokens,{raw=>$1,name=>$1};next;
  }
  $text =~ s/^(.)//s;push @tokens,{raw=>$1};
 }
 return \@tokens;
}

sub module_import_target {
 my($unit,$alias)=@_;
 $alias =~ s/\./__/g;
 return $unit->{imports}{$alias};
}

sub module_receiver {
 my($unit,$target)=@_;
 my $id=substr(sha256_hex('module:'.$target->{name}),0,24);
 unless(exists $unit->{receivers}{$id}){
  my %exports;
  for my $d(values %{$target->{decls}}){
   next unless $d->{public};
   my $kind=$d->{kind};
   $exports{$d->{name}}{$kind}=$d->{linked} if $kind eq 'function' || $kind eq '@' || $kind eq '#';
  }
  $unit->{receivers}{$id}={name=>$target->{name},exports=>\%exports};
 }
 return $id;
}

sub module_resolve {
 my($unit,$kind,$name,$file,$line)=@_;
 $name =~ /^LLSMod_/ and fail("reserved module linker name '$name'",$file,$line);
 my @kinds=$kind eq 'callable'?('function','meta','action_op'):($kind);
 if($name =~ /^($module_name)->($module_name)$/){
  my($alias,$short)=($1,$2);
  my $target=module_import_target($unit,$alias) or fail("unknown module alias '$alias'",$file,$line);
  return module_exported($target,$kind,$short,\@kinds,$file,$line);
 }
 for my $category(@kinds){
  if(my $d=$unit->{decls}{"$category:$name"} // $unit->{opened}{"$category:$name"}){return $d->{linked}}
 }
 for my $alias(sort {length($b)<=>length($a) || $a cmp $b} keys %{$unit->{imports}}){
  next unless index($name,$alias.'__')==0;
  my $short=substr($name,length($alias)+2);my $target=$unit->{imports}{$alias};
  return module_exported($target,$kind,$short,\@kinds,$file,$line);
 }
 return undef;
}

sub module_exported {
 my($target,$kind,$short,$kinds,$file,$line)=@_;
 for my $category(@$kinds){
  my $d=$target->{decls}{"$category:$short"};
  return $d->{linked} if $d && $d->{public};
 }
 fail("module '$target->{name}' does not export ".($kind=~/^[\@#!%]$/?$kind:'').$short,$file,$line);
}

sub module_normalize_name {
 my($unit,$name)=@_;
 return $name if $name =~ /->/; # explicit qualifier, not a legacy dotted name
 for my $alias(sort {length($b)<=>length($a) || $a cmp $b} keys %{$unit->{imports}}){
  (my $dotted=$alias)=~s/__/./g;
  return $alias.'__'.$1 if $name =~ /^\Q$dotted\E\.(.+)$/;
 }
 return $name;
}

# Resolve aliases at the same boundary as exports, after every local header
# and every import is known. A binding stores its target's linked identity;
# no duplicate function, token, state slot, or nominal type is emitted.
sub module_read_alias {
 my($unit,$text,$file,$line)=@_;
 $text =~ s{\s*//.*$}{};
 $text =~ /^\.alias\s+([\@#!%&]?)($module_ref)\s+([\@#!%&]?)([A-Za-z_]\w*)$/
    or fail('expected .alias SOURCE NEW',$file,$line);
 my($fromsig,$from,$sig,$new)=($1,$2,$3,$4);
 ($sig eq $fromsig || $fromsig eq '%' && !$sig)
   or fail('an alias must preserve its symbol category (sigil)',$file,$line);
 my $key=$sig.$new;
 $new =~ /^(?:LLSMod_|__lls_|__LLS_|EOF$|LIT_)/ and fail("reserved alias name '$new'",$file,$line);
 $sig eq '%' && builtin_sigil_type($new) and fail("reserved builtin type '%$new'",$file,$line);
 exists $unit->{aliases}{$key} and fail("duplicate alias '$key'",$file,$line);
 $unit->{aliases}{$key}={from=>$from,sigil=>$fromsig,new_sigil=>$sig,name=>$new,file=>$file,line=>$line};
}

sub alias_scope {
 my($unit,$name)=@_;$name=module_normalize_name($unit,$name);
 if($name =~ /^($module_name)->($module_name)$/){
  my($alias,$short)=($1,$2);
  my $target=module_import_target($unit,$alias);
  return ($target,$short) if $target;
  return ($unit,$name);
 }
 for my $prefix(sort {length($b)<=>length($a) || $a cmp $b} keys %{$unit->{imports}}){
  return ($unit->{imports}{$prefix},substr($name,length($prefix)+2)) if index($name,$prefix.'__')==0;
 }
 return ($unit,$name);
}

sub module_bind_aliases {
 my($unit)=@_;my $aliases=$unit->{aliases}||{};my($bind,$inherit);
 # A legacy `.type %A = %B` has B's constructors too. Record their names
 # for module visibility checks, leaving actual type validation to the compiler.
 $inherit=sub {
  my($d,$seen)=@_;return unless $d->{type_base} && !$seen->{$d->{name}}++;
  my($scope,$short)=alias_scope($unit,$d->{type_base});
  $bind->($aliases->{'%'.$short}) if $scope==$unit && $aliases->{'%'.$short};
  my $base=$scope->{decls}{"%:$short"} || ($scope==$unit && $scope->{opened}{"%:$short"});
  if($base && ($scope==$unit || $base->{public})){
   $inherit->($base,$seen) if $scope==$unit && !$scope->{opened}{"%:$short"};
   $d->{members}=$base->{members};$d->{constructor}=$base->{constructor};
  }
  $d->{constructor}=1 if $scope==$unit && $short =~ /^(?:Unit|Void)$/;
 };
 $bind=sub {
  my($alias)=@_;return if $alias->{bound};
  my($sig,$src,$new,$file,$line)=@$alias{qw(sigil from name file line)};
  $alias->{active}++ and fail("cyclic alias '$sig$new'",$file,$line);
  my $name=module_normalize_name($unit,$src);
  $name =~ /^LLSMod_/ and fail("reserved module linker name '$name'",$file,$line);
  my $local=$aliases->{$sig.$name};
  $local||=$aliases->{($sig eq '&'?'':'&').$name} if !$sig || $sig eq '&';
  $bind->($local) if $local;
  my @kinds=$sig ? ($sig eq '&'?'function':$sig) : qw(function meta grammar ir action_op);
  my($scope,$short)=alias_scope($unit,$src);
  my @matches;
  for my $k(@kinds){
   my $key="$k:$short";my $d=$scope->{decls}{$key};
   $d||=$scope->{opened}{$key} if $scope==$unit;
   push @matches,$d if $d && ($scope==$unit || $d->{public});
  }
  if(!@matches && ($sig eq '%' || !$sig) && $short =~ /^(\w+)\.(\w+)$/){
   my($owner,$member)=($1,$2);
   my $key=($sig eq '%'?'%':'ir').":$owner";
   $bind->($aliases->{$sig.$owner}) if $scope==$unit && $aliases->{$sig.$owner};
   my $d=$scope->{decls}{$key} || ($scope==$unit && $scope->{opened}{$key});
   $inherit->($d,{}) if $d && $scope==$unit && !$scope->{opened}{$key};
   if($d && ($scope==$unit || $d->{public}) && $d->{members}{$member}){
    push @matches,{kind=>($sig eq '%'?'%':'action_op'),linked=>$d->{linked}.'.'.$member,constructor=>1};
   }
  }
  if(!@matches && $scope==$unit){
   push @matches,{kind=>'%',linked=>$name,constructor=>($name eq 'Unit')} if $sig eq '%' && builtin_sigil_type($name);
   push @matches,{kind=>'grammar',linked=>'EOF'} if !$sig && $name eq 'EOF';
   push @matches,{kind=>'function',linked=>$name} if !$sig && $name =~ /^(?:len|pop|top|push|emit|print|final_stack_height|reverse|min|max|abs|is_finite|spawn|join|sleep)$/;
  }
  @matches or fail("unknown or private alias source '$sig$src'",$file,$line);
  @matches==1 or fail("ambiguous alias source '$sig$src'",$file,$line);
  my $d=$matches[0];
  $inherit->($d,{}) if $scope==$unit && !$scope->{opened}{"%:$short"} && $d->{kind} eq '%';
  if($sig eq '%' && !$alias->{new_sigil}){
   $d->{constructor} or fail("alias source '%$src' is not a callable constructor or accessor",$file,$line);
   $d={%$d,kind=>'action_op',linked=>'%'.$d->{linked}};
  }
  my $key="$d->{kind}:$new";
  if($d->{kind} eq 'function' || $d->{kind} eq 'meta' || $d->{kind} eq 'action_op'){
   (grep {$unit->{decls}{"$_:$new"} || $unit->{opened}{"$_:$new"}} qw(function meta action_op))
     and fail("alias '$new' conflicts with a callable declaration",$file,$line);
   $new =~ /^(?:let|return|if|else|while|for|in|on|require|Throw|grab|from|into|call|discard|end|async|fn|await|true|false|nil|None|len|pop|top|push|emit|print|final_stack_height|reverse|min|max|abs|is_finite|spawn|join|sleep)$/
     and fail("reserved callable alias '$new'",$file,$line);
  }
  ($unit->{decls}{$key} || $unit->{opened}{$key}) and fail("alias '$sig$new' conflicts with a declaration",$file,$line);
  my $public=exists($unit->{exports}{($alias->{new_sigil} eq '&'?'':$alias->{new_sigil}).$new})
          || (($unit->{kind}//'') eq 'library' && !keys %{$unit->{exports}});
  $unit->{decls}{$key}={%$d,name=>$new,public=>$public,file=>$file,line=>$line,alias=>1};
  delete $unit->{decls}{$key}{type_base};
  $alias->{bound}=1;
 };
 $bind->($aliases->{$_}) for sort keys %$aliases;
 $inherit->($_,{}) for grep {$_->{type_base}} values %{$unit->{decls}};
}

# A compact Alias:name followed by '(' is a direct module function call in
# actions only. Do not confuse a local's Fn(...) -> ... type with a call.
sub module_colon_call {
 my($tokens,$i,$unit)=@_;
 return if $i+3>$#$tokens;
 my $alias=$tokens->[$i]{name}//'';
 return unless $alias =~ /^$module_name$/ && module_import_target($unit,$alias) &&
               $tokens->[$i+1]{raw} eq ':';
 my $name=$tokens->[$i+2]{name}//'';
 return unless $name =~ /^[A-Za-z_]\w*$/;
 my $arg=$i+3;
 $arg++ while $arg<@$tokens && $tokens->[$arg]{raw} =~ /^\s+$/;
 return if $arg>=@$tokens || $tokens->[$arg]{raw} ne '(';
 if($name eq 'Fn' || $name eq 'fn'){
  my $depth=0;
  for(my $j=$arg;$j<@$tokens;$j++){
   my $raw=$tokens->[$j]{raw};
   $depth++ if $raw eq '(';
   next unless $raw eq ')' && !--$depth;
   $j++;$j++ while $j<@$tokens && $tokens->[$j]{raw} =~ /^\s+$/;
   return if $j<$#$tokens && $tokens->[$j]{raw} eq '-' && $tokens->[$j+1]{raw} eq '>';
   last;
  }
 }
 return ($alias,$name);
}

sub module_rewrite_text {
 my($text,$unit,$mode,$file,$line)=@_;
 my $tokens=module_lex($text,\$unit->{$mode.'_quote'},$mode,$unit);
 for my $i(0..$#$tokens){
  my $t=$tokens->[$i];next unless defined $t->{name};
  my $name=$t->{name};
  $name =~ /^(?:[\@#\$!%&])?LLSMod_/ and fail("reserved module linker name '$name'",$file,$line);
  next if $name =~ /^(?:\$|t')/;
   if($mode eq 'action'){
    my $previous=$i-1;$previous-- while $previous>=0 && $tokens->[$previous]{raw}=~/^\s+$/;
    # Branch captures, metavariables and property names following a
    # parenthesized/indexed receiver do not name module declarations.
    next if $previous>=0 && $tokens->[$previous]{raw} =~ /^(?:on|\.)$/;
    next if $previous>=0 && $tokens->[$previous]{raw} eq '*' &&
            $name =~ /^(?:This|Args|Argv|Type|Constructor)(?:\.|$)/;
    if(($previous<0 || $tokens->[$previous]{raw} ne 'let') &&
       (my($alias,$member)=module_colon_call($tokens,$i,$unit))){
     my $resolved=module_resolve($unit,'function',"${alias}->$member",$file,$line);
     my $receiver=module_receiver($unit,module_import_target($unit,$alias));
     # Preserve direct-call provenance for the action parser and metavar
     # binding without changing the declaration or function-reference name.
     $t->{raw}="LLSMod_colon_${receiver}_".$resolved;
     for my $j($i+1,$i+2){$tokens->[$j]={raw=>''}}
     next;
    }
    if($previous>=0 && $tokens->[$previous]{raw} eq ':' && $name =~ /^[A-Za-z_]\w*$/){
     my $before=$previous-1;
     $before-- while $before>=0 && $tokens->[$before]{raw}=~/^\s+$/;
     # Preserve the method name when a local or an expression is the
     # receiver. Quoted table keys can still be followed by function calls.
     next if $before>=0 && ($tokens->[$before]{name} ||
                            $tokens->[$before]{raw}=~/^(?:\)|\])$/);
    }
    # A known local can have a property whose spelling looks like a
    # legacy dotted module qualifier. Explicit arrow accesses still link.
    next if $name !~ /->/ && $name =~ /^([A-Za-z_]\w*)\./ && $unit->{locals}{$1};
   }
   my($sigil,$bare)=$name =~ /^([\@#!%&]?)(.*)$/;
   $bare=module_normalize_name($unit,$bare);
  my($kind,$suffix)=('','');
  if($mode eq 'grammar'){
   my $next=join('',map {$_->{raw}} @$tokens[$i+1..$#$tokens]);
   next if $next =~ /^\s*:/; # capture labels are not declarations
   $kind='grammar';
    }elsif($sigil eq '%' && $bare =~ /^((?:${module_name}->)?\w+)(\.\w+)$/){($bare,$suffix,$kind)=($1,$2,'%')}
    elsif($sigil ne ''){$kind=$sigil eq '&'?'function':$sigil}
   elsif($bare =~ /^((?:${module_name}->)?\w+)(\.\w+)$/){($bare,$suffix)=($1,$2);$kind='ir'}
  elsif($mode eq 'meta'){$kind='meta'}
  else{$kind='callable'}
  my $resolved=module_resolve($unit,$kind,$bare,$file,$line);
   if(defined $resolved){
    # Without imports an entry function keeps its original spelling. A
    # scalar local of that name must not capture a reference via its alias.
    if($kind eq 'callable' && $unit->{decls}{"function:$bare"} && $unit->{decls}{"function:$bare"}{alias}
        && $unit->{decls}{"function:$resolved"}){
     $sigil='&';
    }
    $t->{raw}=$sigil.$resolved.$suffix;
   }
    elsif(!$unit->{root} && !($kind eq '%' && builtin_sigil_type($bare)) && ($kind eq 'grammar' && $bare ne 'EOF' || $sigil ne '')){
    # An unresolved module reference must never bind to an entry-file symbol.
    # Unresolved bare dotted names may instead be ordinary member accesses.
    fail("undeclared ".($kind eq 'grammar'?'grammar symbol':"symbol $sigil")." '$bare' in module '$unit->{name}'",$file,$line);
  }
 }
 return join('',map {$_->{raw}} @$tokens);
}

sub module_action_locals {
 my($text,$state,$unit)=@_;my $tokens=module_lex($text,$state);
 my @t=grep {defined($_->{name}) || $_->{raw} !~ /^\s+$/} @$tokens;my %locals;
 for my $i(0..$#t){
  my $name=$t[$i]{name}//'';
  if($name eq 'let' || $name eq 'for'){
   $locals{$t[$i+1]{name}//''}=1 if $i<$#t;
  }
   if($name =~ /^[A-Za-z_]\w*$/ && $i<$#t && $t[$i+1]{raw} eq '=' && ($i+2>$#t || $t[$i+2]{raw} ne '=')){
    $locals{$name}=1;
   }
   if($name =~ /^[A-Za-z_]\w*$/ && $i<$#t && $t[$i+1]{raw} eq ':' && !module_colon_call(\@t,$i,$unit)){
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
 my $tokens=module_lex($text,undef,'type',$unit);
 for my $t(@$tokens){
  next unless defined $t->{name};
    if($t->{name} =~ /^%($module_ref)$/){
     my $name=module_normalize_name($unit,$1);
     $t->{raw}='%'.(module_resolve($unit,'%',$name,$file,$line)//$name);
    }elsif($t->{name} =~ /^($module_ref)\.Op$/){
    my $name=module_normalize_name($unit,$1);
    $t->{raw}=(module_resolve($unit,'ir',$name,$file,$line)//$name).'.Op';
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
  if(ends_spec_block($block,$l)){$block='';$facet='';next}
  if($block eq 'rule' && $l =~ /^'(syntax|semantics|lower|analyze|emit)\s*:/){$facet=$1}
  if($block eq 'function'||$block eq 'init'||$block eq 'macro'||($block eq 'rule' && $facet ne 'syntax')){
   (my $action=$entry->[0]) =~ s/^\s*'\w+\s*:\s*//;
    %action_locals=(%action_locals,module_action_locals($action,\$quote,$unit));
  }
 }
 %locals=(%locals,%action_locals);$unit->{locals}=\%locals;
  for my $name(sort keys %locals){
   if($unit->{opened}{"function:$name"} || $unit->{opened}{"meta:$name"}){
    fail("local '$name' shadows a plain import; use qualified or as",$unit->{file},1);
   }
  if(exists($unit->{decls}{"function:$name"}) && $unit->{decls}{"function:$name"}{linked} ne $name){
   fail("module local '$name' shadows a function declaration; use a distinct local name",$unit->{file},1);
  }
  if(exists($unit->{decls}{"meta:$name"}) && $unit->{decls}{"meta:$name"}{linked} ne $name){
   fail("module local '$name' shadows a metaprogram declaration; use a distinct local name",$unit->{file},1);
  }
  ($unit->{opened}{"action_op:$name"} || $unit->{decls}{"action_op:$name"})
    and fail("module local '$name' shadows an action operation alias",$unit->{file},1);
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
     # The default external C symbol is the original declaration's spelling.
     if($l =~ /^\.extern\s+c\b/ && $d->{linked} ne $name && $raw !~ /\bsymbol\s+"[A-Za-z_]\w*"\s*$/){
      $raw.=' symbol "'.$name.'"';
     }
    $raw =~ s/^(\s*\.(?:async\s+function|extern\s+c|\w+)\s+[\@#!%]?)\Q$name\E\b/$1$d->{linked}/;
    }elsif($l =~ /^\.start\s+(?:"($module_ref)"|($module_ref))\s*$/){
    my $name=module_normalize_name($unit,$1//$2);
    $raw='.start '.(module_resolve($unit,'grammar',$name,$file,$line)//$name);
   }elsif($l =~ /^\.precedence\s+(left|right|nonassoc)\s+(.+)$/){
    $raw=".precedence $1 ".module_rewrite_text($2,$unit,'grammar',$file,$line);
    }elsif($l =~ /^\.rewrite\s+(\w+)\s*:\s*($module_ref)\s*$/){
    my($name,$ir)=($1,module_normalize_name($unit,$2));
    $name=module_prefix($unit).$name unless $unit->{root};
    $raw=".rewrite $name: ".(module_resolve($unit,'ir',$ir,$file,$line)//$ir);
    }elsif($l =~ /^\.backend\s+(\w+)\s+for\s+($module_ref)\s*$/){
    my($target,$ir)=($1,module_normalize_name($unit,$2));
    $raw=".backend $target for ".(module_resolve($unit,'ir',$ir,$file,$line)//$ir);
   }
   $block=module_block($l);
   }elsif(ends_spec_block($block,$l)){
   $block='';$facet='';
  }elsif($block eq 'rule'){
   if($l =~ /^'(syntax|semantics|lower|analyze|emit)\s*:\s*(.*)$/){
    $facet=$1;$raw="'$facet: ".module_rewrite_text($2,$unit,$facet eq 'syntax'?'grammar':'action',$file,$line);
   }elsif($l eq "'end"){$facet=''}
   elsif($facet){$raw=module_rewrite_text($raw,$unit,$facet eq 'syntax'?'grammar':'action',$file,$line)}
   }elsif($block eq 'function' || $block eq 'init' || $block eq 'macro'){
   $raw=module_rewrite_text($raw,$unit,'action',$file,$line);
   }elsif($block eq 'ir' || $block eq 'sumtype' || $block eq 'prodtype'){
   $raw=module_rewrite_types($raw,$unit,$file,$line);
  }elsif($block eq 'rewrite' && $l =~ /^\.when\s+(.+)$/){
   $raw='.when '.module_rewrite_text($1,$unit,'action',$file,$line);
  }
  $entry->[0]=$raw;
 }
}
1;
