LIB = asdl.lib.new "std.graph.directed"

local G={}
function G.new(nodes,edges) local g={nodes={},out={},incoming={}} for _,v in ipairs(nodes or {}) do G.add(g,v) end for _,e in ipairs(edges or {}) do G.edge(g,e[1],e[2]) end return g end
function G.add(g,v) assert(v~=nil); if not g.out[v] then g.nodes[#g.nodes+1]=v;g.out[v]={};g.incoming[v]={} end return g end
function G.edge(g,a,b) G.add(g,a);G.add(g,b); for _,v in ipairs(g.out[a]) do if v==b then return g end end; table.insert(g.out[a],b);table.insert(g.incoming[b],a);return g end
function G.successors(g,v) return assert(g.out[v],'unknown vertex') end
function G.predecessors(g,v) return assert(g.incoming[v],'unknown vertex') end
function G.reverse(g) local r=G.new(g.nodes);for _,v in ipairs(g.nodes) do for _,w in ipairs(g.out[v]) do G.edge(r,w,v) end end return r end
LIB.Graph=G

asdl.signlib(LIB)
return LIB
