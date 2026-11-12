// SYNTAX TEST "Packages/QScript/QScript.sublime-syntax"
use math, mylib::util;
// <- keyword.control.import
//  ^^^^ support.module.stdlib
//        ^^^^^^^^^^^ entity.name.namespace
//                   ^ punctuation.terminator
const LIMIT = 0x1F_FF;
// <- storage.modifier
//    ^^^^^ entity.name.constant
//            ^^^^^^^ constant.numeric.integer.hexadecimal
fn hyp(a, b) { return isqrt(a * a + b * b); }
// <- storage.type.function
// ^^^ entity.name.function
//     ^ variable.parameter
//      ^ punctuation.separator.parameters
//           ^ punctuation.section.block.begin
//             ^^^^^^ keyword.control.flow
//                    ^^^^^ support.function.stdlib
//                            ^ keyword.operator.arithmetic
//                                          ^ punctuation.section.block.end
my total = math::gcd(LIMIT, 12) ** 2;   # TODO tune
// <- storage.modifier
// ^^^^^ variable.other.readwrite.declaration
//         ^^^^ support.module
//             ^^ punctuation.accessor.double-colon
//               ^^^ variable.function
//                   ^^^^^ variable.other.constant
//                              ^^ keyword.operator.arithmetic
//                                      ^ comment.line punctuation.definition.comment
//                                        ^^^^ comment.line.todo
next if total % 2 == 0;
// <- keyword.control.flow
//   ^^ keyword.control.conditional
//                ^^ keyword.operator.comparison
say 'a', '\n', 12ab, "nope", true and not false;
// <- support.function.builtin
//  ^^^ constant.character
//       ^^^^ constant.character
//        ^^ constant.character.escape
//             ^^^^ invalid.illegal.numeric
//                   ^^^^^^ invalid.illegal.string
//                           ^^^^ constant.language.boolean
//                                ^^^ keyword.operator.word
x <<= 3; y **= 2;
//^^^ keyword.operator.assignment.augmented
//         ^^^ keyword.operator.assignment.augmented
fn f() { while (1) { last if 0; } }
//       ^^^^^ keyword.control.loop
//                   ^^^^ keyword.control.flow
//             ^ meta.group punctuation.section.group.begin
loop { say -2 ** 2 >> 1; }  // done
// <- keyword.control.loop
//                 ^^ keyword.operator.bitwise
//                          ^^ comment.line
//                             ^^^^ comment.line
/* block
   comment */ loop { last; }
// <- comment.block
//            ^^^^ keyword.control.loop
//                   ^^^^ keyword.control.flow
for (my i = 0; i < 3; i++) say i;
// <- keyword.control.loop
//   ^^ storage.modifier
//      ^ variable.other.readwrite.declaration
//           ^ punctuation.terminator
//                       ^ punctuation.section.group.end
