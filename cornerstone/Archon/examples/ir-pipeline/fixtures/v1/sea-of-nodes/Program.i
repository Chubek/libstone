/* Archon Program.i fixture: Sea-of-Nodes schema v1.
 *
 * IDs are dense and declarations are ordered.  This minimal graph has one
 * pinned start node and no inputs.
 */
ARCHON_IR_PROGRAM_BEGIN(ARCHON_IR_PROGRAM_SCHEMA, 1u, 0u);
ARCHON_IR_NODE(
    0u,
    ARCHON_IR_NODE_START,
    0u,
    ARCHON_IR_FLAG_PINNED,
    ARCHON_IR_NODE_NONE,
    0u,
    0u);
ARCHON_IR_ROOT(0u);
ARCHON_IR_PROGRAM_END();
