package LLSynthActionSemantics;

use strict;
use warnings;
use Digest::SHA qw(sha256_hex);
use Encode ();
use File::Spec ();
use File::Temp qw(tempdir);
use IPC::Open3 qw(open3);

my $ident=qr/[A-Za-z_][A-Za-z_0-9]{0,63}/;
my %sort=(Int=>'integer',String=>'identifier');

sub is_header { return $_[0] =~ /^'semantics\s+actionsemantics\b/ }

sub parse_header {
 my($rule,$raw,$file,$line)=@_;
 if($rule->{action_semantics} || exists $rule->{facets}{semantics}){
  main::fail("cannot combine or repeat ordinary and ActionSemantics 'semantics facets",$file,$line);
 }
 $raw =~ /^\s*'semantics\s+actionsemantics\s*\(([^()]*)\)\s*->\s*(Int|String|Void)\s*:\s*(.*)$/
  or main::fail("expected 'semantics actionsemantics(capture: Int|String, ...) -> Int|String|Void:",$file,$line);
 my($params,$returns,$inline)=($1,$2,$3);
 my(@fields,%seen);
 if($params =~ /\S/){
  for my $param(split /,/,$params,-1){
   $param =~ /^\s*($ident)\s*:\s*(Int|String)\s*$/
    or main::fail('ActionSemantics captures require an ASCII name and type Int or String',$file,$line);
   my($name,$type)=($1,$2);
   $seen{$name}++ and main::fail("duplicate ActionSemantics capture '$name'",$file,$line);
   push @fields,{name=>$name,type=>$type};
  }
 }
 @fields <= 16 or main::fail('ActionSemantics supports at most 16 captures per rule',$file,$line);
 $rule->{action_semantics}={file=>$file,line=>$line,fields=>\@fields,returns=>$returns,body=>[]};
 push @{$rule->{action_semantics}{body}},[$inline,$file,$line] if length $inline;
}

sub validate_captures {
 my($spec,$tokens,$rules)=@_;
 for my $rule(@{$spec->{rules}}){
  my $facet=$rule->{action_semantics} or next;
  my @productions=grep {$_->{lhs} eq $rule->{name}} @{$spec->{productions}};
  for my $field(@{$facet->{fields}}){
   my($name,$want)=@$field{qw(name type)};
   for my $p(@productions){
    my($i)=grep {$p->{labels}[$_] eq $name} 0..$#{$p->{labels}};
    defined $i or main::fail("ActionSemantics capture '$name' must occur in every syntax alternative",@$facet{qw(file line)});
    my $symbol=$p->{rhs}[$i];
    my $got=$tokens->{$symbol}//$rules->{$symbol}//'Void';
    # Optional and grouped captures can yield nil or vectors even when one
    # branch looks scalar; they are deliberately not part of this bridge.
    $got='a grouped, repeated or optional value' if exists $spec->{internal}{$symbol};
    $got eq $want || $got eq 'Any' or
     main::fail("ActionSemantics capture '$name' expects $want, got $got",@$facet{qw(file line)});
   }
  }
 }
}

sub find_tool {
 my($requested,$facet)=@_;
 my @choices=$requested =~ m{[/\\]} || File::Spec->file_name_is_absolute($requested)
  ? ($requested) : map {File::Spec->catfile($_,$requested)} File::Spec->path;
 for my $path(@choices){return main::canonical_path($path) if -f $path && -x $path}
 main::fail("ActionSemantics generator '$requested' not found or not executable; use --acsem PATH",@$facet{qw(file line)});
}

sub compile {
 my($spec,$options)=@_;
 my @facets=map {$_->{action_semantics}} grep {$_->{action_semantics}} @{$spec->{rules}};
 return unless @facets;
 my $first=$facets[0];
 my $target=$options->{T} eq 'python3'?'python':'c';
 @facets <= 256 or main::fail('ActionSemantics supports at most 256 delegated rules',@$first{qw(file line)});
 my @source=([ 'language LLSynthActions {', @$first{qw(file line)} ],
             [ '  start Facet;', @$first{qw(file line)} ],
             [ '  syntax Facet =', @$first{qw(file line)} ]);
 for my $i(0..$#facets){
  my $facet=$facets[$i];
  my $ctor=$facet->{constructor}='LLSFacet'.($i+1);
  my $fields=join(', ',map {"$_->{name}: $sort{$_->{type}}"} @{$facet->{fields}});
  push @source,[ '    '.$ctor."($fields)".($i==$#facets?';':' |'), @$facet{qw(file line)} ];
 }
 for my $facet(@facets){
  push @source,["  rule $facet->{constructor} = (", @$facet{qw(file line)}];
  for my $entry(@{$facet->{body}}){
   my($line,$file,$number)=@$entry;
   # The inline facet contains only an action, not syntax/data/rewrite
   # declarations or other equations. Semicolons only occur in declarations.
   my($fragment)=split /[#]|\/\//,$line,2;
   defined($fragment) && $fragment =~ /;/ and
    main::fail('ActionSemantics facet expects an action without a terminating semicolon',$file,$number);
   push @source,$entry;
  }
  push @source,['  );', @{$source[-1]}[1,2]];
 }
 push @source,['}', @{$source[-1]}[1,2]];
 my $tool=find_tool($options->{acsem}//'acsem',$first);
 my $temp=tempdir('llsynth-actionsem-XXXXXXXX',TMPDIR=>1,CLEANUP=>1);
 my($src,$generated,$err)=map {File::Spec->catfile($temp,$_)} qw(facets.acsem generated acsem.log);
 open my $input,'>:raw',$src or die "cannot write temporary ActionSemantics specification: $!\n";
 print {$input} Encode::encode('UTF-8',join('',map {$_->[0]."\n"} @source))
  or die "cannot write temporary ActionSemantics specification: $!\n";
 close $input or die "cannot close temporary ActionSemantics specification: $!\n";
 open my $errors,'>:raw',$err or die "cannot open ActionSemantics diagnostic stream: $!\n";
 my $log='>&'.fileno($errors);
 my $pid=eval {open3(undef,$log,$log,$tool,'--target',$target,'-o',$generated,'--',$src)};
 defined $pid or main::fail("cannot run ActionSemantics generator '$tool': $@",@$first{qw(file line)});
 close $errors;
 if(waitpid($pid,0) != $pid || $?){
  my $error=main::slurp($err)//'';chomp $error;
  if($error =~ /^acsem: \Q$src\E:(\d+):\d+: (.*)$/s){
   my($index,$message)=($1-1,$2);
   my $entry=$source[$index]||$source[-1];
   main::fail("ActionSemantics: $message",@$entry[1,2]);
  }
  $error =~ s/^acsem: //;
  main::fail('ActionSemantics generator failed'.(length($error)?": $error":''),@$first{qw(file line)});
 }
 my $code=main::slurp($generated)//'';
 $code=$target eq 'python'?python_bridge($code,$first):c_bridge($code,$first);
 for my $facet(@facets){delete @$facet{qw(body file line)}}
 my %deps=map {$_->{path}=>$_} @{$spec->{dependencies}};
 $deps{$tool}={path=>$tool,sha256=>sha256_hex(main::slurp($tool))};
 $spec->{dependencies}=[map {$deps{$_}} sort keys %deps];
 return \$code;
}

sub python_bridge {
 my($code,$first)=@_;
 # Reuse the complete generated action machine, replacing only its stdin/stdout
 # CLI. A function scope isolates its tables and helper names from LLSynth.
 $code =~ s/\ndef main\(\):\n.*\z/\n/s
  or main::fail('unrecognized NovoStone Python output (missing ActionSemantics CLI entry point)',@$first{qw(file line)});
 $code.=<<'PY';
def invoke(facet, captures):
    args = []
    import re
    for field in facet['fields']:
        name, kind = field['name'], field['type']
        value = captures[name]
        if kind == 'Int':
            if type(value) is not int or not -(1 << 31) <= value < (1 << 31):
                raise LanguageError('ACSEM', f"capture '{name}' must fit a signed 32-bit integer")
            args.append(str(value))
        else:
            if not isinstance(value, str) or re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]{0,63}', value) is None:
                raise LanguageError('ACSEM', f"capture '{name}' must be an ASCII identifier of up to 64 bytes")
            args.append(value)
    term = facet['constructor'] + '(' + ','.join(args) + ')'
    try:
        machine = Machine(term)
        root = machine.node(START)
        status, data, _ = machine.run(root)
        if machine.error or status == 3:
            raise Limit()
    except (Limit, MemoryError, RecursionError, ValueError):
        raise LanguageError('ACSEM', 'ActionSemantics input or resource error') from None
    if status:
        raise LanguageError('ACSEM', 'ActionSemantics action ' + ('failed' if status == 1 else 'escaped'))
    values = machine.objects[data] if data >= 0 else []
    returns = facet['returns']
    if returns == 'Void' and not values:
        return None
    if returns == 'Int' and len(values) == 1 and values[0][0] == VK_INT:
        return values[0][1]
    if returns == 'String' and len(values) == 1 and values[0][0] == VK_TOKEN:
        return machine.symbols[values[0][1]]
    raise LanguageError('ACSEM', f'ActionSemantics completed with a result incompatible with {returns}')
return invoke
PY
 $code =~ s/^(\S|[ \t]+\S)/    $1/gm;
 return "def _lls_make_action_semantics():\n".$code.
        "_lls_action_semantics = _lls_make_action_semantics()\ndel _lls_make_action_semantics\n";
}

sub c_bridge {
 my($code,$first)=@_;
 $code =~ s/\nint main\(void\) \{\n.*\z/\n/s
  or main::fail('unrecognized NovoStone C output (missing ActionSemantics CLI entry point)',@$first{qw(file line)});
 $code.=<<'C';
/* Bridge to LLSynth: kind 0 = Void, 1 = Int, 2 = String.
   Status 0 = completed, 1 = failed, 2 = escaped, 3 = resource/input error,
   4 = result kind/arity mismatch. No heap identity crosses the bridge. */
int LLS_ACTION_SEMANTICS_ENTRY(const char *term, int kind, int64_t *number, char text[65]) {
    int code = 3;
    Machine *m = calloc(1, sizeof(*m));
    if (!m) return code;
    m->steps = STEPS_MAX;
    m->length = strlen(term);
    if (m->length <= 65536) {
        char trailing[65];
        memcpy(m->input, term, m->length);
        for (int j = 0; j < TOKEN_COUNT; ++j) (void)intern(m, TOKENS[j]);
        int root = parse_node(m, START, 0);
        token(m, trailing);
        if (!m->error && !*trailing) {
            Pending result = run(m, root);
            if (!m->error) {
                code = result.status;
                if (code == 0) {
                    const Object *data = result.data >= 0 ? m->objects[result.data] : NULL;
                    code = 4;
                    if (kind == 0 && (!data || data->count == 0)) {
                        code = 0;
                    } else if (data && data->count == 1) {
                        Value value = data->items[0];
                        if (kind == 1 && value.kind == VK_INT) {
                            *number = value.imm;
                            code = 0;
                        } else if (kind == 2 && value.kind == VK_TOKEN) {
                            memcpy(text, m->symbols[value.imm], 65);
                            code = 0;
                        }
                    }
                }
            }
        }
    }
    for (int j = 0; j < HEAP_MAX; ++j) free(m->objects[j]);
    free(m);
    return code;
}
C
 return $code;
}

sub c_adapter {
 my($entry)=@_;
 my $code=<<'C';
#ifdef __cplusplus
extern "C" int LLS_ACTION_SEMANTICS_ENTRY(const char *, int, int64_t *, char *);
#else
extern int LLS_ACTION_SEMANTICS_ENTRY(const char *, int, int64_t *, char *);
#endif
static int acsem_identifier(const char *s) {
 size_t n=0;
 for(;*s;s++,n++){
  if(n==64 || !((*s>='A'&&*s<='Z')||(*s>='a'&&*s<='z')||*s=='_'||(n&&*s>='0'&&*s<='9')))return 0;
 }
 return n!=0;
}
static Val acsem_eval(Context *c,Json *facet,Map *caps){
 /* At most 16 arguments, each at most 64 bytes plus one separator. */
 char term[64+16*65],text[65];
 size_t used=(size_t)snprintf(term,sizeof(term),"%s(",jg(facet,"constructor"));
 Json *fields=jo(facet,"fields");
 Val out=LLS_ZERO;
 for(int i=0;i<jlen(fields);i++){
  Json *field=ji(fields,i);const char *name=jg(field,"name");
  Val input=map_get(caps,name);
  if(i)term[used++]=',';
  if(!strcmp(jg(field,"type"),"Int")){
   if(input.tag!=VINT||input.i<INT32_MIN||input.i>INT32_MAX){
    error(c,"ACSEM: capture '%s' must fit a signed 32-bit integer",name);return out;
   }
   used+=(size_t)snprintf(term+used,sizeof(term)-used,"%" PRId64,input.i);
  }else{
   if(input.tag!=VSTR||!acsem_identifier(input.s)){
    error(c,"ACSEM: capture '%s' must be an ASCII identifier of up to 64 bytes",name);return out;
   }
   size_t size=strlen(input.s);memcpy(term+used,input.s,size);used+=size;
  }
 }
 term[used++]=')';term[used]=0;
 const char *type=jg(facet,"returns");
 int kind=!strcmp(type,"Void")?0:!strcmp(type,"Int")?1:2;
 int64_t number=0;
 switch(LLS_ACTION_SEMANTICS_ENTRY(term,kind,&number,text)){
 case 0:
  if(kind==1)return vint(number);
  if(kind==2)return vstr(dup_n(c,text,strlen(text)));
  break;
 case 1:error(c,"ACSEM: ActionSemantics action failed");break;
 case 2:error(c,"ACSEM: ActionSemantics action escaped");break;
 case 4:error(c,"ACSEM: ActionSemantics completed with a result incompatible with %s",type);break;
 default:error(c,"ACSEM: ActionSemantics input or resource error");break;
 }
 return out;
}
C
 $code =~ s/\bLLS_ACTION_SEMANTICS_ENTRY\b/$entry/g;
 return $code;
}

1;
