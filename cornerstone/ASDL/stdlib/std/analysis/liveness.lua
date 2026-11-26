LIB = asdl.lib.new "std.analysis.liveness"

local S=asdl.lib.require('std.collections.set').Set
local D=asdl.lib.require('std.analysis.dataflow')
local C=asdl.lib.require('std.cfg')
local G=asdl.lib.require('std.graph.directed').Graph
-- Phi operands are read on incoming edges, not in the destination block.
function LIB.Liveness(cfg)
 local g=C.Graph(cfg)
 local function transfer(v,out)
  local a=S.copy(out)
  local xs=C.Instructions(cfg.blocks[v])
  for i=#xs,1,-1 do
   for _,d in ipairs(xs[i].defs or {}) do a[d]=nil end
   for _,u in ipairs(xs[i].uses or {}) do a[u]=true end
  end
  for _,p in ipairs(cfg.blocks[v].phis or {}) do a[p.dest]=nil end
  return a
 end
 local function edge(from,to,live)
  local a=S.copy(live)
  for _,p in ipairs(cfg.blocks[to].phis or {}) do a[assert(p.args[from],'missing phi operand')]=true end
  return a
 end
 return D.Backward(g,{bottom=function()return {}end,join=S.union,equal=S.equal,transfer=transfer,edge=edge})
end
function LIB.LiveIn(c) local a=LIB.Liveness(c);return a end
function LIB.LiveOut(c) local _,b=LIB.Liveness(c);return b end
function LIB.Interference(c)
 local _,out=LIB.Liveness(c)
 local g=G.new()
 local function connect(a,b) if a~=b then G.edge(g,a,b);G.edge(g,b,a) else G.add(g,a) end end
 local function clique(a) for x in pairs(a) do G.add(g,x);for y in pairs(a) do connect(x,y) end end end
 for _,v in ipairs(c.order) do
  local a=S.copy(out[v]);clique(a)
  local xs=C.Instructions(c.blocks[v])
  for i=#xs,1,-1 do
   local x=xs[i]
   for _,d in ipairs(x.defs or {}) do G.add(g,d);for u in pairs(a) do connect(d,u) end end
   for _,d in ipairs(x.defs or {}) do a[d]=nil end
   for _,u in ipairs(x.uses or {}) do G.add(g,u);a[u]=true end
   clique(a)
  end
  for _,p in ipairs(c.blocks[v].phis or {}) do G.add(g,p.dest);for u in pairs(a) do connect(p.dest,u) end end
 end
 return g
end

asdl.signlib(LIB)
return LIB
