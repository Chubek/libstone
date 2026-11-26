LIB = asdl.lib.new "std.semantics.bindings"

local S=asdl.lib.require('std.scope').Scope
LIB.Scope=S
function LIB.Resolve(scope,name,node) local b=S.resolve(scope,name);if not b then return nil end;if node then asdl.annotate(node,'binding-id',b.id) end;return b end
function LIB.MarkWrite(binding) binding.written=true;return binding end
function LIB.MarkRead(binding) binding.read=true;return binding end
function LIB.Unused(scope) local r={} for _,b in ipairs(scope.ordered) do if not b.read then r[#r+1]=b end end;return r end

asdl.signlib(LIB)
return LIB
