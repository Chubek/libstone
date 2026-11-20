%start entry
%skip /[ \t]+/

entry:
    'long:' /a|ab+/ '!'                         { $$ = $2; }
  | 'rollback:' /a|abc/ 'b' '!'                 { $$ = $2; }
  | 'zero:' /a*/ '!'                            { $$ = $2; }
  | 'class:' /[[:alpha:]][^[:digit:]]\d\w[^x]{1,2}/ 'x' { $$ = $2; }
  | 'dot:' /./ '!'                              { $$ = $2; }
  | 'eof:' /end$/                               { $$ = $2; }
  | 'eof:' /end$/ '!'                           { $$ = $2; }
  | 'bol:' /(^pick)|pre(^fix)/ '!'               { $$ = $2; }
  | 'word:' /\bcat\b/ /[!A-Z]/                   { $$ = $2; }
  | 'inside:x' /\Bcat\B/ 'z'                    { $$ = $2; }
  ;
