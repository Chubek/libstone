LIB = asdl.lib.new "std.semantics.inheritance"

-- C3 linearization with cycle and inconsistent-order detection.

function LIB.Linearize(classes,name)
    local cache,active={},{};
    local function lin(n)
        if cache[n] then
            return cache[n]
        end;
        assert(not active[n],'inheritance cycle: '..n);
        active[n]=true;
        local ps=assert(classes[n],'unknown class '..n);
        local seq={} for _,p in ipairs(ps) do
            local a={} for i,v in ipairs(lin(p)) do
                a[i]=v
            end;
            seq[#seq+1]=a
        end;
        local a={} for i,p in ipairs(ps) do
            a[i]=p
        end;
        seq[#seq+1]=a;
        local out={n};
        while true do
            local candidate,nonempty;
            for _,s in ipairs(seq) do
                if #s>0 then
                    nonempty=true;
                    local h=s[1];
                    local valid=true;
                    for _,t in ipairs(seq) do
                        for i=2,#t do
                            if t[i]==h then
                                valid=false
                            end
                        end
                    end;
                    if valid then
                        candidate=h;
                        break
                    end
                end
            end;
            if not nonempty then
                break
            end;
            assert(candidate,'inconsistent inheritance order');
            out[#out+1]=candidate;
            for _,s in ipairs(seq) do
                if s[1]==candidate then
                    table.remove(s,1)
                end
            end
        end;
        active[n]=nil;
        cache[n]=out;
        return out
    end;
    return lin(name)
end

function LIB.Lookup(classes,methods,name,method)
    for _,n in ipairs(LIB.Linearize(classes,name)) do
        if methods[n] and methods[n][method]~=nil then
            return methods[n][method],n
        end
    end
end

asdl.signlib(LIB)
return LIB
