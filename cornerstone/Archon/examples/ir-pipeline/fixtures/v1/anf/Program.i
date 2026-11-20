/* Archon Program.i fixture: ANF schema v1.
 *
 * This is an include fragment, not a translation unit.  IR.c defines the
 * statement-like IR_* macros before including it.
 */
IR_PROGRAM_BEGIN("add-one");
IR_FUNCTION_BEGIN(main, "main", IR_TYPE_I32);
IR_PARAM(x, "x", IR_TYPE_I32);
IR_BLOCK_BEGIN(entry, "entry");
IR_LET_BEGIN(sum, IR_TYPE_I32, IR_PRIM_ADD, IR_EFFECT_NONE);
IR_ARG_VALUE(x);
IR_ARG_INT(IR_TYPE_I32, 1);
IR_LET_END();
IR_RETURN(sum);
IR_BLOCK_END();
IR_FUNCTION_END();
IR_PROGRAM_END();
