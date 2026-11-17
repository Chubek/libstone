# Modules status

Neither reusable specification imports nor synthesized-language module registries
are implemented. Each `.acsem` input is one self-contained language definition.
No module resolution or arbitrary filesystem reads occur during specification
evaluation. Imports, aliases, separate linking, initialization cycles, interfaces
and functors remain planned and are not accepted keywords.

A future resolver must default to an in-memory registry, require explicit
filesystem capabilities, preserve source spans, reject unresolved dependencies,
and define cycle and initialization semantics before backend generation.
