LIB = asdl.lib.new "std.tree"

local Tree = {}

-- enter returns false to prune a subtree. Children are in schema order.
function Tree.walk(root, enter, leave)
    local function visit(node, parent)
        if enter and enter(node, parent) == false then return end
        for _, child in ipairs(asdl.children(node)) do visit(child, node) end
        if leave then leave(node, parent) end
    end
    visit(root, nil)
end

function Tree.collect(root, predicate, descend)
    local result = {}
    Tree.walk(root, function(node, parent)
        if predicate(node, parent) then result[#result + 1] = node end
        if descend and not descend(node, parent) then return false end
    end)
    return result
end

function Tree.any(root, predicate)
    if predicate(root) then return true end
    for _, child in ipairs(asdl.children(root)) do
        if Tree.any(child, predicate) then return true end
    end
    return false
end

function Tree.fold(root, initial, combine)
    local acc = initial
    Tree.walk(root, nil, function(node) acc = combine(acc, node) end)
    return acc
end

LIB.Tree = Tree
asdl.signlib(LIB)
return LIB
