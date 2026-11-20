/* Instruction-selector MIR schema v1.
 *
 * This compact stream exercises the selector's small-producer decoder. The
 * same selector also accepts the scheduler's checked MachineIR v2 schema.
 */
ARCHON_MIR_BEGIN(1)
ARCHON_MIR_VALUE(1u, 32u, 0u, 0u)
ARCHON_MIR_CONSTANT(2u, 12u, 5u, 0u)
ARCHON_MIR_NODE(3u, "add", 32u, 0u, 0u)
ARCHON_MIR_OPERAND(3u, 1u)
ARCHON_MIR_OPERAND(3u, 2u)
ARCHON_MIR_ROOT(3u)
ARCHON_MIR_END()
