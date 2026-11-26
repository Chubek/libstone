LIB = asdl.lib.new "std.collections.priorityqueue"

local Q={}
function Q.new(less)
    return {a={},less=less or function(a,b)
        return a<b
    end}
end

function Q.push(q,x)
    local a=q.a;
    a[#a+1]=x;
    local i=#a;
    while i>1 do
        local p=i//2;
        if not q.less(a[i],a[p]) then
            break
        end;
        a[i],a[p]=a[p],a[i];
        i=p
    end;
    return q
end

function Q.pop(q)
    local a=q.a;
    if #a==0 then
        return nil
    end;
    local r=a[1];
    local last=table.remove(a);
    if #a>0 then
        a[1]=last;
        local i=1;
        while i*2<=#a do
            local j=i*2;
            if j+1<=#a and q.less(a[j+1],a[j]) then
                j=j+1
            end;
            if not q.less(a[j],a[i]) then
                break
            end;
            a[i],a[j]=a[j],a[i];
            i=j
        end
    end;
    return r
end

function Q.peek(q)
    return q.a[1]
end

LIB.PriorityQueue=Q

asdl.signlib(LIB)
return LIB
