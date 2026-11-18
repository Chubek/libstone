/* Descriptive grammar of the raw WASM.aml stream in lowercase hex.
 * Each hex pair is one byte of the actual stream; the C++ codec consumes
 * bytes, not hex text, and does not use a generated parser.
 */
${declare longest_match Stream}
Stream: Instruction*;
Instruction: BareOpcode | ConstOpcode Sleb7;
BareOpcode: '00' | '01' | '1a' | '1b' | '45' | '50'
          | '67' | '68' | '69' | '6a' | '6b' | '6c'
          | '71' | '72' | '73' | '74' | '75' | '76' | '77' | '78'
          | '79' | '7a' | '7b' | '7c' | '7d' | '7e'
          | '83' | '84' | '85' | '86' | '87' | '88' | '89' | '8a';
ConstOpcode: '41' | '42';
Sleb7: "[0-7][0-9a-f]";
whitespace: "[ \t\r\n]*";
