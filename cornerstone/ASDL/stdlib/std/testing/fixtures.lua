LIB = asdl.lib.new "std.testing.fixtures"

function LIB.Node(ty,tag,fields)
    return asdl.make(ty,tag,fields or {})
end

function LIB.Run(cases)
    local result={passed=0,failed=0,errors={}};
    for _,c in ipairs(cases) do
        local ok,e=pcall(c.run);
        if ok then
            result.passed=result.passed+1
        else
            result.failed=result.failed+1;
            result.errors[#result.errors+1]={name=c.name,error=tostring(e)}
        end
    end;
    return result
end

asdl.signlib(LIB)
return LIB
