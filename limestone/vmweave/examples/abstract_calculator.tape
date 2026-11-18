# 20 * 2; branch if false; write state, run a concrete handler, read result.
PUSH 20 PUSH 2 MUL DUP PUSH 40 EQ JZ &wrong
SAVE BIAS LOAD JUMP &end
wrong: POP PUSH 0
end: HALT
