LIB = asdl.lib.new "std.serialize"

local NULL={};
LIB.Null=NULL
local array_mt={};
function LIB.Array(xs)
    return setmetatable(xs or {},array_mt)
end

local function quote(s)
    assert(utf8.len(s),'invalid UTF-8 string');
    return '"'..s:gsub('[%z\1-\31\\"]',function(c)
        if c=='\\' then
            return '\\\\'
        elseif c=='"' then
            return '\\"'
        end;
        return string.format('\\u%04x',c:byte())
    end)..'"'
end

local function arraylike(t)
    if getmetatable(t)==array_mt then
        return true
    end;
    local n=0 for k in pairs(t) do
        if math.type(k)~='integer' or k<1 then
            return false
        end;
        n=n+1
    end;
    return n>0 and n==#t
end

local decimal_point=string.format('%.1f',1.5):sub(2,-2)
local function number(v)
    assert(v==v and math.abs(v)~=math.huge,'nonfinite JSON number')
    if math.type(v)=='integer' then return tostring(v) end
    local s=string.format('%.17g',v)
    if decimal_point~='.' then
        local i,j=s:find(decimal_point,1,true)
        if i then s=s:sub(1,i-1)..'.'..s:sub(j+1) end
    end
    if not s:find('[.eE]') then s=s..'.0' end
    return s
end

function LIB.JSON(value,options)
    local seen={};
    options=options or {};
    local enc
    local function seq(a)
        local r={} local n=0;
        for k in pairs(a) do
            assert(math.type(k)=='integer' and k>=1 and k<=#a,'invalid array key');
            n=n+1
        end;
        assert(n==#a,'sparse array');
        for i,x in ipairs(a) do
            r[i]=enc(x)
        end;
        return '['..table.concat(r,',')..']'
    end

    enc=function(v)
        if v==nil or v==NULL then
            return 'null'
        end;
        local t=type(v);
        if t=='string' then
            return quote(v)
        elseif t=='boolean' then
            return tostring(v)
        elseif t=='number' then
            return number(v)
        end;
        assert(not seen[v],'cyclic JSON value');
        seen[v]=true;
        local out
        if asdl.isnode(v) then
            local parts={'"$type":'..quote(asdl.type(v))};
            local tag=asdl.tag(v);
            if tag then
                parts[#parts+1]='"$tag":'..quote(tag)
            end;
            for _,f in ipairs(asdl.fields(v)) do
                assert(f.name~='$type' and f.name~='$tag' and f.name~='$annotations','reserved serialization key');
                parts[#parts+1]=quote(f.name)..':'..((f.cardinality=='*' or f.cardinality=='+') and seq(f.value) or enc(f.value))
            end;
            if options.annotations and asdl.annotations then
                parts[#parts+1]='"$annotations":'..enc(asdl.annotations(v))
            end;
            out='{'..table.concat(parts,',')..'}'
        elseif t=='table' then
            if arraylike(v) then
                out=seq(v)
            else
                local keys,parts={},{};
                for k in pairs(v) do
                    assert(type(k)=='string','JSON object key must be string');
                    keys[#keys+1]=k
                end;
                table.sort(keys);
                for i,k in ipairs(keys) do
                    parts[i]=quote(k)..':'..enc(v[k])
                end;
                out='{'..table.concat(parts,',')..'}'
            end

        else
            error('unsupported JSON value '..t)
        end;
        seen[v]=nil;
        return out
    end

    return enc(value)
end

function LIB.DecodeJSON(s,opts)
    opts=opts or {};
    local i=1;
    local function ws()
        local _,j=s:find('^[ \t\r\n]*',i);
        i=j+1
    end;
    local function expect(c)
        ws();
        assert(s:sub(i,i)==c,'expected '..c..' at byte '..i);
        i=i+1
    end

    local function str()
        expect('"');
        local r={};
        while i<=#s do
            local c=s:sub(i,i);
            i=i+1;
            if c=='"' then
                local v=table.concat(r);
                assert(utf8.len(v),'invalid UTF-8');
                return v
            elseif c=='\\' then
                local e=s:sub(i,i);
                i=i+1;
                local map={['"']='"',['\\']='\\',['/']='/',b='\b',f='\f',n='\n',r='\r',t='\t'};
                if e=='u' then
                    local function hex()
                        local h=s:sub(i,i+3);
                        assert(#h==4 and h:match('^%x%x%x%x$'),'bad unicode escape');
                        i=i+4;
                        return tonumber(h,16)
                    end;
                    local n=hex();
                    if n>=0xd800 and n<=0xdbff then
                        assert(s:sub(i,i+1)=='\\u','missing low surrogate');
                        i=i+2;
                        local l=hex();
                        assert(l>=0xdc00 and l<=0xdfff,'bad low surrogate');
                        n=0x10000+(n-0xd800)*1024+l-0xdc00
                    else
                        assert(n<0xdc00 or n>0xdfff,'unexpected low surrogate')
                    end;
                    r[#r+1]=utf8.char(n)
                else
                    assert(map[e],'invalid JSON escape');
                    r[#r+1]=map[e]
                end
            else
                assert(c:byte()>=32,'control in string');
                r[#r+1]=c
            end
        end;
        error('unterminated string')
    end

    local parse;
    parse=function(depth)
        assert(depth<=(opts.depth or 256),'JSON nesting limit');
        ws();
        local c=s:sub(i,i);
        if c=='"' then
            return str()
        elseif c=='{' then
            i=i+1;
            local r={};
            ws();
            if s:sub(i,i)=='}' then
                i=i+1;
                return r
            end;
            repeat
                local k=str();
                assert(r[k]==nil,'duplicate JSON key');
                expect(':');
                r[k]=parse(depth+1);
                ws();
                local x=s:sub(i,i);
                i=i+1;
                if x=='}' then
                    return r
                end;
                assert(x==',','expected comma or }')
            until false
        elseif c=='[' then
            i=i+1;
            local r=LIB.Array();
            ws();
            if s:sub(i,i)==']' then
                i=i+1;
                return r
            end;
            repeat
                r[#r+1]=parse(depth+1);
                ws();
                local x=s:sub(i,i);
                i=i+1;
                if x==']' then
                    return r
                end;
                assert(x==',','expected comma or ]')
            until false
        else
            for _,p in ipairs({{'true',true},{'false',false},{'null',NULL}}) do
                if s:sub(i,i+#p[1]-1)==p[1] then
                    i=i+#p[1];
                    return p[2]
                end
            end;
            local start=i;
            if s:sub(i,i)=='-' then
                i=i+1
            end;
            if s:sub(i,i)=='0' then
                i=i+1
            else
                local a,b=s:find('^[1-9]%d*',i);
                assert(a,'invalid number at '..i);
                i=b+1
            end;
            if s:sub(i,i)=='.' then
                i=i+1;
                local a,b=s:find('^%d+',i);
                assert(a,'invalid fraction');
                i=b+1
            end;
            if s:sub(i,i):match('[eE]') then
                i=i+1;
                if s:sub(i,i):match('[+-]') then
                    i=i+1
                end;
                local a,b=s:find('^%d+',i);
                assert(a,'invalid exponent');
                i=b+1
            end;
            local n=tonumber(s:sub(start,i-1));
            assert(n and math.abs(n)~=math.huge,'invalid number');
            return n
        end
    end

    local v=parse(0);
    ws();
    assert(i>#s,'trailing JSON data');
    return v
end

function LIB.FromJSON(s,options)
    options=options or {};
    local function go(v)
        if v==NULL or type(v)~='table' then
            return v
        end;
        if v['$type']~=nil then
            assert(type(v['$type'])=='string','invalid node type')
            assert(v['$tag']==nil or type(v['$tag'])=='string','invalid node tag')
            local fields={} for k,x in pairs(v) do
                if k~='$type' and k~='$tag' and k~='$annotations' then
                    if x~=NULL then
                        if getmetatable(x)==array_mt then
                            for _,item in ipairs(x) do assert(item~=NULL,'null in AST sequence') end
                        end
                        fields[k]=go(x)
                    end
                end
            end;
            local n=asdl.make(v['$type'],v['$tag'],fields);
            if options.annotations and v['$annotations'] then
                for k,x in pairs(v['$annotations']) do
                    asdl.annotate(n,k,go(x))
                end
            end;
            return n
        end;
        local r=getmetatable(v)==array_mt and LIB.Array() or {}
        for k,x in pairs(v) do
            r[k]=go(x)
        end;
        return r
    end;
    return go(LIB.DecodeJSON(s,options))
end

function LIB.SExpr(value)
    local seen={};
    local enc;
    local function seq(v)
        local a={} for i,x in ipairs(v) do
            a[i]=enc(x)
        end;
        return '('..table.concat(a,' ')..')'
    end;
    enc=function(v)
        if v==nil or v==NULL then
            return '#none'
        elseif type(v)=='boolean' then
            return v and '#t' or '#f'
        elseif type(v)=='string' then
            return quote(v)
        elseif type(v)=='number' then
            return tostring(v)
        end;
        assert(not seen[v],'cyclic S-expression');
        seen[v]=true;
        local out;
        if asdl.isnode(v) then
            local a={asdl.tag(v) or asdl.type(v)};
            for _,f in ipairs(asdl.fields(v)) do
                a[#a+1]='('..f.name..' '..((f.cardinality=='*' or f.cardinality=='+') and seq(f.value) or enc(f.value))..')'
            end;
            out='('..table.concat(a,' ')..')'
        elseif type(v)=='table' and arraylike(v) then
            out=seq(v)
        else
            error('unsupported S-expression value')
        end;
        seen[v]=nil;
        return out
    end;
    return enc(value)
end

-- Versioned length-framed UTF-8 JSON transport. Not Zephyr pickling or native memory serialization.

function LIB.Binary(node,options)
    local s=LIB.JSON(node,options);
    return 'ASDL\1'..string.pack('>I8',#s)..s
end

function LIB.FromBinary(s,options)
    assert(s:sub(1,5)=='ASDL\1','bad binary header');
    local n,pos=string.unpack('>I8',s,6);
    assert(n>=0 and n==#s-pos+1,'invalid binary size');
    return LIB.FromJSON(s:sub(pos),options)
end

asdl.signlib(LIB)
return LIB
