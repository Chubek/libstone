LIB = asdl.lib.new "std.testing.snapshot"

-- Caller supplies storage callbacks; no implicit filesystem writes.

function LIB.Snapshot(key,value,read,write,update)
    local old=read(key);
    if update then
        assert(write,'snapshot writer required');
        write(key,value);
        return true
    end;
    if old==value then
        return true
    end;
    return false,{key=key,expected=old,actual=value}
end

asdl.signlib(LIB)
return LIB
