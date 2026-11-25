package main;
use strict;
use warnings;
use Digest::SHA qw(sha256_hex);
use Encode ();
use File::Spec ();
use File::Temp qw(tempfile);
use IPC::Open3 qw(open3);
use JSON::PP ();

# Ask VMWeave for the runtime and the contract of the very same lowered module.
# Lua can execute arbitrary code, so evaluate the embedded chunk only once.
sub vmgen_bundle {
 my($decl)=@_;
 my($file,$line)=@$decl{qw(file line)};
 my $cli=$ENV{VMWEAVE_EXECUTABLE} || 'vmweave-cli';
 my @candidates=$cli =~ m{[/\\]} || File::Spec->file_name_is_absolute($cli)
   ? ($cli) : map {File::Spec->catfile($_,$cli)} File::Spec->path;
 my($executable)=grep {-f $_ && -x _} @candidates;
 defined $executable
   or fail("cannot find VMWeave ($cli); set VMWEAVE_EXECUTABLE to vmweave-cli",$file,$line);
 $cli=canonical_path($executable);
 my($lua,$path)=tempfile('llsynth-vmgen-XXXXXXXX',SUFFIX=>'.lua',TMPDIR=>1,UNLINK=>1);
 binmode $lua;
 print $lua Encode::encode('UTF-8',("\n" x $line).$decl->{body}) or die "$path: $!\n";
 close $lua or die "$path: $!\n";
 my($err)=tempfile('llsynth-vmgen-errors-XXXXXXXX',TMPDIR=>1,UNLINK=>1);
 binmode $err;
 my($in,$out);
 my $pid=eval {open3($in,$out,'>&'.fileno($err),$cli,'--emit-c-bundle','--source-name',$file,File::Spec->rel2abs($path))};
 $@ and fail("cannot launch VMWeave ($cli); set VMWEAVE_EXECUTABLE to vmweave-cli: $@",$file,$line);
 close $in;
 binmode $out;
 local $/;
 my $response=<$out>;
 close $out;
 waitpid($pid,0)==$pid or fail("cannot wait for VMWeave ($cli): $!",$file,$line);
 if($?!=0){
  my $status=$?;
  seek $err,0,0 or die "cannot read VMWeave diagnostics: $!\n";
  my $detail=strip_ws(Encode::decode('UTF-8',scalar(<$err>)//'',Encode::FB_DEFAULT));
  fail('VMWeave failed: '.($detail || ($status & 127 ? 'terminated by signal '.($status & 127) : 'exit status '.($status >> 8))),$file,$line);
 }
 my $bundle=eval {JSON::PP->new->utf8->decode($response//'')};
 ref($bundle) eq 'HASH' && ($bundle->{schema}//'') eq 'limestone.vmweave.c-bundle' && ($bundle->{version}//0)==1
  or fail('expected VMWeave C bundle version 1 (check the CLI version and that Lua writes nothing to stdout)',$file,$line);
 return ($bundle,$cli);
}

sub prepare_vmgen {
 my($spec)=@_;
 my $decl=delete $spec->{vmgen_decl} or return;
 my($ir,$file,$line)=@$decl{qw(ir file line)};
 my $error=sub {fail($_[0],$file,$line)};
 (grep {$_->{target} eq 'vmweave'} @{$spec->{backends_compiled}})
  and $error->("a .backend vmweave conflicts with .vmgen");
 my @ops=sort grep {index($_,$ir.'.')==0} keys %{$spec->{op_by_name}};
 unless(@ops){
  $error->(".vmgen requires a nonempty IR '$ir'");
 }
 my($bundle,$cli)=vmgen_bundle($decl);
 my($name,$cell,$execution,$capacity,$components,$words)=@$bundle{qw(name cell execution stack_capacity components words)};
 defined($name) && $name =~ /\A[A-Za-z][A-Za-z0-9_]*\z/ && ref($components) eq 'ARRAY' && ref($words) eq 'ARRAY'
  && defined($capacity) && $capacity =~ /\A[0-9]+\z/ && $capacity>=1 && $capacity<=1048576
  && length($bundle->{c_source}//'') && index($bundle->{c_source},"\0")==-1
  or $error->('invalid VMWeave C bundle');
 ($cell//'') eq 'int64_t' or $error->('.vmgen requires VMWeave int64_t cells');
 ($execution//'') eq 'switch' or $error->('.vmgen requires VMWeave switch dispatch');
 my %components=map {$_=>1} @$components;
 $components{insncode} && $components{dispatch} or $error->('.vmgen requires the insncode and dispatch VMWeave components');
 my %words=map {$_->{name}=>$_} @$words;
 my %opcodes;
 for my $op(@ops){
  my $short=substr($op,length($ir)+1);
  my $info=$spec->{op_by_name}{$op};
  my $word=$words{$short} or $error->("no VMWeave instruction named '$short' for $op");
  ($word->{flow}//'') eq 'next' or $error->("$op requires a VMWeave instruction with next control flow");
  my @params=comma_parts($info->{params});
  !(grep {$_ !~ /:\s*Int\s*$/} @params) && !(grep {$_ ne 'Int'} (@{$info->{inputs}},@{$info->{outputs}}))
   or $error->("$op requires Int operands and Int stack types for .vmgen");
  ref($word->{operands}) eq 'ARRAY' && @params==@{$word->{operands}}
    && !(grep {$_ ne 'cell'} @{$word->{operands}})
   or $error->("operand count or kind mismatch between $op and VMWeave instruction '$short'");
  $word->{pop}==$info->{pop} && $word->{push}==$info->{push}
   or $error->("stack effect mismatch between $op and VMWeave instruction '$short'");
  $word->{opcode}=~/\A[0-9]+\z/ && $word->{opcode}<=2147483647 or $error->('invalid VMWeave opcode');
  $opcodes{$op}=$word->{opcode};
 }
 my $prologue=$bundle->{c_source}.<<C;

#line 1 "<LLSynth VM runner>"
#include <stdio.h>
#include <inttypes.h>
int main(void) {
  static ${name}_vm vm;
  static ${name}_instruction data[] = {
C
 my $epilogue=<<"C";
  };
  const size_t count = sizeof data / sizeof data[0];
  const ${name}_tape tape = {data, count, count};
  ${name}_init(&vm);
  int status = ${name}_run(&vm, &tape, count);
  if (status) {
    fprintf(stderr, "VMWeave: status %d at instruction %zu\\n", status, vm.vw_pc);
    return 1;
  }
  if (vm.vw_halted || vm.vw_pc != count || vm.vw_sp != 1) {
    fputs("VMWeave: expected a complete tape and one result\\n", stderr);
    return 1;
  }
  printf("%" PRId64 "\\n", vm.vw_stack[0]);
  return 0;
}
C
 $spec->{vmgen}={opcodes=>\%opcodes,stack_capacity=>0+$capacity,prologue=>$prologue,epilogue=>$epilogue};
 my %deps=map {$_->{path}=>$_} @{$spec->{dependencies}};
 $deps{$cli}={path=>$cli,sha256=>sha256_hex(slurp($cli))};
 $spec->{dependencies}=[map {$deps{$_}} sort keys %deps];
}

1;
