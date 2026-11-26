LIB = asdl.lib.new "std.match.structural"

-- Match patterns are ordinary tables: Capture(name, optional constraint), Node(type,tag,fields), Any().

function LIB.Capture(name,p)
    return {kind='capture',name=name,pattern=p}
end

function LIB.Any()
    return {kind='any'}
end

function LIB.Node(ty,tag,fields)
    return {kind='node',type=ty,tag=tag,fields=fields or {}}
end

local function eq(a,b)
    if a==b then
        return true
    end;
    if asdl.isnode(a) and asdl.isnode(b) then
        if asdl.type(a)~=asdl.type(b) or asdl.tag(a)~=asdl.tag(b) then
            return false
        end;
        local af,bf=asdl.fields(a),asdl.fields(b);
        if #af~=#bf then
            return false
        end;
        for i,f in ipairs(af) do
            if f.name~=bf[i].name or not eq(f.value,bf[i].value) then
                return false
            end
        end;
        return true
    end;
    if type(a)=='table' and type(b)=='table' and not asdl.isnode(a) and not asdl.isnode(b) then
        for k,v in pairs(a) do
            if not eq(v,b[k]) then
                return false
            end
        end;
        for k in pairs(b) do
            if a[k]==nil then
                return false
            end
        end;
        return true
    end;
    return false
end

LIB.Equal=eq
function LIB.Match(p,v)
    local captures,seen={},{};
    local function m(x,y)
        if type(x)=='table' and x.kind=='any' then
            return true
        elseif type(x)=='table' and x.kind=='capture' then
            if x.pattern and not m(x.pattern,y) then
                return false
            end;
            if seen[x.name] then
                return eq(captures[x.name],y)
            end;
            seen[x.name]=true;
            captures[x.name]=y;
            return true
        elseif type(x)=='table' and x.kind=='node' then
            if not asdl.isnode(y) or asdl.type(y)~=x.type or asdl.tag(y)~=x.tag then
                return false
            end;
            local fields={} for _,f in ipairs(asdl.fields(y)) do
                fields[f.name]=f
            end;
            for k,z in pairs(x.fields) do
                if not fields[k] or not m(z,fields[k].value) then
                    return false
                end
            end;
            return true
        elseif type(x)=='table' and type(y)=='table' and not asdl.isnode(x) and not asdl.isnode(y) then
            if #x~=#y then
                return false
            end;
            for k,z in pairs(x) do
                if not m(z,y[k]) then
                    return false
                end
            end;
            for k in pairs(y) do
                if x[k]==nil then
                    return false
                end
            end;
            return true
        end;
        return eq(x,y)
    end;
    if m(p,v) then
        return captures
    end
end

function LIB.Replace(root,p,build)
    local R=asdl.lib.require('std.rewrite').Rewrite;
    return R.bottom_up(root,function(n)
        local c=LIB.Match(p,n);
        if c then
            return build(c,n)
        end
    end)
end

asdl.signlib(LIB)
return LIB
