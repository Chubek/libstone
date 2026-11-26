LIB = asdl.lib.new "std.match.unification"

local U=asdl.lib.require('std.types.unification')
LIB.Var=U.Var;LIB.Term=U.Term;LIB.Unify=U.Unify;LIB.Apply=U.Apply

asdl.signlib(LIB)
return LIB
