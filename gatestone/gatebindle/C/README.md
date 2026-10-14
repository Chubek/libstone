# C frontend reference bundle

The manifest assembles six GateML fragments:

- `Profile.gml`: frontend identity and metadata.
- `Lexer.gml`: decimal integers, operators, parentheses, and C comments.
- `Parser.gml`: layered expression grammar with explicit precedence.
- `AST.gml`: labeled node kinds.
- `Adapter.gml`: semantic Jiterati lowering hooks.
- `Build.gml`: facility metadata for future generators.

Example source: `12 + 3 * (4 - 2)`. Its root is `Add`; multiplication and
grouped subtraction are nested according to C expression precedence. Lowering
creates a zero-argument i64 Jiterati function with subtraction, multiplication,
addition, and return instructions.

This bundle establishes the frontend packaging pattern. Full C declarations,
preprocessing, types, integer promotions, symbol resolution, and translation
units are future language extensions.
