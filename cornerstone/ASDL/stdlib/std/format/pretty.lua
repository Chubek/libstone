LIB = asdl.lib.new "std.format.pretty"

function LIB.Text(s)
    assert(not s:find('\n',1,true),'Text cannot contain newline');
    return {tag='text',s=s}
end

function LIB.Line()
    return {tag='line'}
end

function LIB.Concat(...)
    return {tag='cat',items={...}}
end

function LIB.Nest(n,d)
    assert(n>=0);
    return {tag='nest',n=n,doc=d}
end

function LIB.Group(d)
    return {tag='group',doc=d}
end

function LIB.Join(sep,docs)
    local a={} for i,d in ipairs(docs) do
        if i>1 then
            a[#a+1]=sep
        end;
        a[#a+1]=d
    end;
    return LIB.Concat(table.unpack(a))
end

-- Wadler-style group fitting against remaining output, measuring bytes.

function LIB.Render(doc,width)
    width=width or 80;
    local out,col={},0;
    local stack={{doc,0,false}};
    local function push(s,d,n,f)
        if d.tag=='cat' then
            for i=#d.items,1,-1 do
                s[#s+1]={d.items[i],n,f}
            end
        elseif d.tag=='nest' then
            s[#s+1]={d.doc,n+d.n,f}
        else
            s[#s+1]={d.doc,n,f}
        end
    end

    local function fits(rem,s)
        while rem>=0 and #s>0 do
            local x=table.remove(s);
            local d,n,f=table.unpack(x);
            if d.tag=='text' then
                rem=rem-#d.s
            elseif d.tag=='line' then
                if not f then
                    return true
                end;
                rem=rem-1
            else
                push(s,d,n,f)
            end
        end;
        return rem>=0
    end

    while #stack>0 do
        local x=table.remove(stack);
        local d,n,f=table.unpack(x);
        if d.tag=='text' then
            out[#out+1]=d.s;
            col=col+#d.s
        elseif d.tag=='line' then
            if f then
                out[#out+1]=' ';
                col=col+1
            else
                out[#out+1]='\n'..string.rep(' ',n);
                col=n
            end
        elseif d.tag=='group' then
            local test={} for i,v in ipairs(stack) do
                test[i]=v
            end;
            test[#test+1]={d.doc,n,true};
            stack[#stack+1]={d.doc,n,f or fits(width-col,test)}
        else
            push(stack,d,n,f)
        end
    end;
    return table.concat(out)
end

asdl.signlib(LIB)
return LIB
