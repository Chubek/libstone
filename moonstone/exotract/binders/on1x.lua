-- The stable Exotract custom binder extension point.
-- Receive an Internals table and return a new or modified Internals table.
-- This backend intentionally yields typed CDecl + unsafe On1x import metadata,
-- not speculative native On1x source syntax.
return function(internals)
  assert(type(internals) == "table")
  assert(internals.unsafe == true, "unreviewed FFI cannot be marked safe")
  assert(type(internals.cdecl) == "string")
  assert(type(internals.on1x) == "string")
  assert(type(internals.implementation) == "string")
  return internals
end
