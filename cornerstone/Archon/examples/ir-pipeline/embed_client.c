/* Minimal argv-free client for the generated ArchonEmbed API. */
#include "ArchonEmbed.h"

#include <stdio.h>

int main(void) {
    ArchonEmbedOptions options;
    ArchonEmbedResult result;
#ifdef ARCHON_ORACLE_ENABLED
    ArchonOracleOptions oracle;
#endif

    archon_embed_options_init(&options);
#ifdef ARCHON_ORACLE_ENABLED
    archon_oracle_options_init(&oracle);
#endif
    options.rewrite = ARCHON_EMBED_REWRITE_EGRAPH;
    options.dialect = ARCHON_EMBED_DIALECT_GAS;
    options.output_path = "Program.embed.s";
    options.artifact_policy = ARCHON_EMBED_ARTIFACTS_DISCARD;
#ifdef ARCHON_ORACLE_ENABLED
    options.oracle = &oracle; /* Explicitly inactive: socket_path is NULL. */
#endif

    if (archon_embed_run(&options, &result) != ARCHON_EMBED_STATUS_OK) {
        (void)fprintf(
            stderr, "embedded pipeline failed at stage %u: %s\n",
            result.stage_valid ? (unsigned)result.stage : 0u,
            result.diagnostic);
        return 1;
    }
    return 0;
}
