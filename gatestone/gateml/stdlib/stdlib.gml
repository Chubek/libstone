(* GateML standard-library entry point.
   The build embeds these files in this order (no runtime source-tree lookup):
   Prelude.gml, Int.gml, Bool.gml, Pair.gml, Option.gml, Result.gml,
   String.gml, List.gml, IO.gml.
   The public standard_library() API returns the complete concatenated source.
   Evaluating this file itself returns unit; --no-stdlib disables auto-loading. *)
()
