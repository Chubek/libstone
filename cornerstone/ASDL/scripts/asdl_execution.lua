-- Trusted runtime code. The embedding host installs this once, before loading
-- extension libraries, and retains the returned controller privately.
return function(asdl)
    local pack, unpack = table.pack, table.unpack
    local isnode, typename, tagof = asdl.isnode, asdl.type, asdl.tag
    local make, map_children, clone = asdl.make, asdl.map_children, asdl.clone
    local annotate, annotation, annotations = asdl.annotate, asdl.annotation, asdl.annotations
    local diagnostic = asdl.diagnostic
    local installed, refinements = {}, {}
    local origins = setmetatable({}, {__mode = "k"})
    local main, absent, default, fault = {}, {}, {}, {}
    local integer, float, negzero = {}, {}, {}
    local sessions = setmetatable({}, {__mode = "k"})
    local controller = {}

    local function thread()
        local co, ismain = coroutine.running()
        return ismain and main or co
    end

    local function session()
        return sessions[thread()]
    end

    local function label(node)
        local tag = tagof(node)
        return typename(node) .. (tag and "." .. tag or "")
    end

    local function failure(state, node, code, message)
        if not state.failure then
            state.failure = {
                [fault] = true, severity = "error", code = code,
                message = ("[%s %s] %s"):format(state.phase, label(node), message),
                location = asdl.location(node),
            }
        end
        error(state.failure, 0)
    end

    local function checked_call(state, node, code, fn, ...)
        local result = pack(pcall(fn, ...))
        if not result[1] then
            local err = result[2]
            if type(err) == "table" and rawget(err, fault) then error(err, 0) end
            local message = type(err) == "string" and err or ("Lua raised a " .. type(err))
            failure(state, node, code, message)
        end
        -- A user pcall must not turn a failed checked dispatch into a commit.
        if state.failure then error(state.failure, 0) end
        return unpack(result, 2, result.n)
    end

    local function row(map, node)
        local values = map[node]
        if not values then values = {}; map[node] = values end
        return values
    end

    function asdl.annotate(node, key, value)
        assert(isnode(node), "annotation requires a live AST node")
        assert(type(key) == "string", "annotation key must be a string")
        local state = session()
        if not state then return annotate(node, key, value) end
        if state.proof then
            failure(state, node, "ASDL_REFINEMENT", "a refinement may not write annotations")
        end
        if state.memo_depth > 0 then
            failure(state, node, "ASDL_LOGISTICS", "a memoized hook may not write annotations")
        end
        if value == nil then value = absent end
        row(state.writes, node)[key] = value
    end

    function asdl.annotation(node, key)
        local state = session()
        local writes = state and state.writes[node]
        local val = writes and writes[key]
        if val == absent then return nil end
        if val ~= nil then return val end
        return annotation(node, key)
    end

    if annotations then
        function asdl.annotations(node)
            local result = {}
            for k, v in pairs(annotations(node) or {}) do result[k] = v end
            local state = session()
            for k, v in pairs(state and state.writes[node] or {}) do
                if v == absent then result[k] = nil else result[k] = v end
            end
            return result
        end
    end

    local function record_origin(node, origin)
        if node and origin and isnode(origin) and not rawequal(node, origin) then
            local state = session()
            if not origins[node] and not (state and state.origins[node]) then
                -- Inverting an existing derivation would create a provenance cycle.
                local parent = origin
                while parent and not rawequal(parent, node) do
                    parent = (state and state.origins[parent]) or origins[parent]
                end
                if not parent then
                    if state then state.origins[node] = origin else origins[node] = origin end
                end
            end
        end
        return node
    end

    function asdl.origin(node)
        assert(isnode(node), "origin requires a live AST node")
        local state = session()
        return (state and state.origins[node]) or origins[node]
    end

    function asdl.make(ty, tag, fields, origin)
        return record_origin(make(ty, tag, fields, origin), origin)
    end

    function asdl.map_children(node, fn)
        return record_origin(map_children(node, fn), node)
    end

    if clone then
        function asdl.clone(node)
            return record_origin(clone(node), node)
        end
    end

    local emit = diagnostic.emit
    function diagnostic.emit(record)
        local state = session()
        if state and (state.proof or state.memo_depth > 0) then
            local which = state.proof and "a refinement" or "a memoized hook"
            failure(state, state.current, state.proof and "ASDL_REFINEMENT" or "ASDL_LOGISTICS",
                which .. " may not emit diagnostics")
        end
        return emit(record)
    end

    local function metadata_names(names, kind)
        if names == nil then return nil end
        assert(type(names) == "table", "expected " .. kind .. " names")
        local seen, n = {}, 0
        for i, name in pairs(names) do
            assert(math.type(i) == "integer" and i > 0 and i <= #names,
                "expected a dense array of " .. kind .. " names")
            assert(type(name) == "string" and name ~= "" and not seen[name],
                "duplicate or invalid " .. kind .. " name")
            seen[name], n = true, n + 1
        end
        assert(n == #names, "expected a dense array of " .. kind .. " names")
        return n
    end

    local function name_or_nil(name)
        return name == nil or (type(name) == "string" and name ~= "")
    end

    local function check_registration(spec, fn)
        assert(not session(), "cannot register callbacks during execution")
        assert(type(spec) == "table" and type(fn) == "function", "expected callback specification and function")
        assert(spec.type and name_or_nil(spec.type), "invalid AST type")
        assert(name_or_nil(spec.tag), "invalid constructor name")
    end

    function controller.register(spec, fn)
        check_registration(spec, fn)
        assert(name_or_nil(spec.phase), "invalid logistics phase")
        assert(spec.traversal == nil or spec.traversal == "explicit" or spec.traversal == "children",
            "invalid logistics traversal")
        assert(spec.memoize == nil or type(spec.memoize) == "boolean", "invalid memoize flag")
        local inherited = metadata_names(spec.inherited, "inherited")
        local results = metadata_names(spec.synthesized, "synthesized")
        local phases = row(installed, spec.phase or "default")
        local tags = row(phases, spec.type)
        local tag = spec.tag or default
        assert(not tags[tag], "duplicate logistics hook")
        tags[tag] = {
            fn = fn, traversal = spec.traversal, memoize = spec.memoize,
            inherited = inherited, results = results,
        }
    end

    function controller.register_refinement(spec, fn)
        check_registration(spec, fn)
        assert(spec.field and name_or_nil(spec.field), "invalid refinement field")
        assert(spec.name and name_or_nil(spec.name), "invalid refinement name")
        local fields = row(row(refinements, spec.type), spec.tag or default)
        local list = row(fields, spec.field)
        for _, rule in ipairs(list) do assert(rule.name ~= spec.name, "duplicate refinement") end
        list[#list + 1] = {fn = fn, name = spec.name}
    end

    local globals = {
        assert = assert, error = error, ipairs = ipairs, next = next, pairs = pairs,
        pcall = pcall, select = select, tonumber = tonumber, tostring = tostring, type = type,
        math = math, string = string, table = table, utf8 = utf8,
    }

    function controller.load_schema(schema)
        local files = {}
        local function environment(file)
            local pair = files[file]
            if pair then return pair[1], pair[2] end
            local imports = {asdl = asdl}
            local env = setmetatable({}, {
                __index = function(_, key)
                    local value = imports[key]
                    if value ~= nil then return value end
                    return globals[key]
                end,
                __newindex = function(_, key) error("cannot write a schema hook global: " .. tostring(key), 2) end,
                __metatable = false,
            })
            imports._G = env
            files[file] = {env, imports}
            return env, imports
        end
        for _, import in ipairs(schema.imports) do
            local _, imports = environment(import.file)
            local lib = asdl.lib.require(import.module)
            local function bind(name, value)
                assert(imports[name] == nil, "duplicate or reserved Lua import " .. name)
                assert(value ~= nil, "missing Lua import " .. name)
                imports[name] = value
            end
            if import.alias then bind(import.alias, lib) end
            for _, item in ipairs(import.names) do bind(item.alias or item.name, lib[item.name]) end
        end
        for _, hook in ipairs(schema.logistics) do
            local env = environment(hook.file)
            local params = {}
            for i, name in ipairs(hook.parameters) do
                assert(name:match("^[a-zA-Z_][a-zA-Z_0-9]*$"), "invalid Lua parameter name")
                params[i] = name
            end
            assert(#params > 0, "logistics hook needs an AST parameter")
            local source = ("\n"):rep(hook.line - 1)
                .. "return function(" .. table.concat(params, ",") .. ") " .. hook.body .. "\nend"
            local chunk, message = load(source, "@" .. hook.file, "t", env)
            assert(chunk, message)
            if hook.check_inherited then
                hook.inherited = {}
                for i = 2, #params do hook.inherited[i - 1] = params[i] end
                -- Child traversal appends its packed results after those values.
                if hook.traversal == "children" then table.remove(hook.inherited) end
            end
            controller.register(hook, chunk())
        end
        for _, rule in ipairs(schema.refinements) do
            local _, imports = environment(rule.file)
            local lib, member = rule.name:match("^([^.]+)%.([^.]+)$")
            local fn
            if lib then
                local namespace = imports[lib]
                if type(namespace) == "table" then fn = namespace[member] end
            else
                fn = imports[rule.name]
            end
            assert(type(fn) == "function", ("%s:%d: missing or non-function refinement export %s")
                :format(rule.file, rule.line, rule.name))
            controller.register_refinement(rule, fn)
        end
    end

    local function entries(index, node)
        return index and index[typename(node)]
    end

    local function select_hook(state, node)
        local tags = entries(installed[state.phase], node)
        if tags then return tags[tagof(node)] or tags[default] end
    end

    local function depth(state, node)
        state.depth = state.depth + 1
        if state.depth > state.depth_limit then
            failure(state, node, "ASDL_DEPTH", "AST execution recursion limit exceeded")
        end
    end

    -- Trie keyed by AST identity and Lua inherited-value identity. A call
    -- tuple has a separate result slot, so () is not the same as (nil).
    local function memo_slot(state, node, args)
        if state.memo_entries >= 4096 then return nil end
        local values = row(state.memo, node)
        for i = 1, args.n do
            local arg = args[i]
            if arg == nil then arg = absent end
            if type(arg) == "number" then
                if arg ~= arg then return nil end
                if math.type(arg) == "integer" then
                    values = row(values, integer)
                else
                    values = row(values, float)
                    if arg == 0 and 1 / arg < 0 then arg = negzero end
                end
            end
            values = row(values, arg)
        end
        return values
    end

    local dispatch
    local function visit(state, node, args)
        local results = {}
        for i, child in ipairs(asdl.children(node)) do
            results[i] = pack(dispatch(state, child, unpack(args, 1, args.n)))
        end
        return results
    end

    function asdl.logistics_children(node, ...)
        local state = assert(session(), "logistics_children requires an active execution")
        if not isnode(node) then
            failure(state, state.current, "ASDL_LOGISTICS", "logistics_children requires a live AST node")
        end
        if state.proof then
            failure(state, node, "ASDL_REFINEMENT", "refinements cannot dispatch logistics")
        end
        return visit(state, node, pack(...))
    end

    function asdl.logistics_context()
        local state = session()
        return state and state.context
    end

    function dispatch(state, node, ...)
        if state.failure then error(state.failure, 0) end
        if not isnode(node) then
            failure(state, state.current, "ASDL_LOGISTICS", "logistics requires a live AST node")
        end
        if state.proof then
            failure(state, node, "ASDL_REFINEMENT", "refinements cannot dispatch logistics")
        end
        local hook = select_hook(state, node)
        local args = pack(...)
        depth(state, node)
        local previous = state.current
        state.current = node
        local result, slot
        if hook and hook.inherited and hook.inherited ~= args.n then
            failure(state, node, "ASDL_ARITY", ("expected %d inherited values, got %d"):format(hook.inherited, args.n))
        end
        if hook then
            if state.memo_depth > 0 and not hook.memoize then
                failure(state, node, "ASDL_LOGISTICS", "a memoized hook cannot call an impure hook")
            end
            if hook.memoize then
                slot = memo_slot(state, node, args)
                result = slot and slot[default]
                state.memo_depth = state.memo_depth + 1
            end
            if not result then
                if hook.traversal == "children" then
                    -- Child results are supplied separately after inherited args.
                    local children = visit(state, node, args)
                    args.n = args.n + 1
                    args[args.n] = children
                end
                result = pack(checked_call(state, node, "ASDL_LOGISTICS", hook.fn, node,
                    unpack(args, 1, args.n)))
                if hook.results and hook.results ~= result.n then
                    failure(state, node, "ASDL_ARITY", ("expected %d synthesized values, got %d"):format(hook.results, result.n))
                end
                if slot and state.memo_entries < 4096 then
                    state.memo_entries = state.memo_entries + 1
                    slot[default] = result
                end
            end
            if hook.memoize then state.memo_depth = state.memo_depth - 1 end
        else
            -- With no callback, walk through the type; analysis results are
            -- not interpreted as AST replacements.
            visit(state, node, args)
            result = pack(node)
        end
        state.current = previous
        state.depth = state.depth - 1
        return unpack(result, 1, result.n)
    end

    local function refine_tree(state, node)
        depth(state, node)
        local previous = state.current
        state.current = node
        local tags = entries(refinements, node)
        local common = tags and tags[default]
        local tag = tagof(node)
        local specific = tags and tags[tag]
        local fields, changed = {}, false
        for _, field in ipairs(asdl.fields(node)) do
            local function refine(value)
                if value == nil then return nil end
                if isnode(value) then value = refine_tree(state, value) end
                for i = 1, 2 do
                    local rules
                    if i == 1 then rules = common else rules = specific end
                    for _, rule in ipairs(rules and rules[field.name] or {}) do
                        state.proof = true
                        local result, why = checked_call(state, node, "ASDL_REFINEMENT", rule.fn,
                            value, node, field.name)
                        state.proof = false
                        if result == false or result == nil then
                            failure(state, node, "ASDL_REFINEMENT", rule.name .. " rejected " .. field.name
                                .. (type(why) == "string" and (": " .. why) or ""))
                        elseif result ~= true then
                            if not isnode(value) or not isnode(result) or typename(result) ~= typename(value) then
                                failure(state, node, "ASDL_REFINEMENT", rule.name
                                    .. " must return a Boolean or an AST replacement of the same type")
                            end
                            record_origin(result, value)
                            if not rawequal(result, value) then
                                -- Validate newly introduced descendants and refinements.
                                -- It also bounds nonterminating rewriting rules.
                                result = refine_tree(state, result)
                            end
                            value = result
                        end
                    end
                end
                return value
            end
            if field.cardinality == "*" or field.cardinality == "+" then
                local seq = {}
                for i, v in ipairs(field.value) do
                    seq[i] = refine(v)
                    changed = changed or not rawequal(v, seq[i])
                end
                fields[field.name] = seq
            else
                local v = refine(field.value)
                fields[field.name] = v
                changed = changed or not rawequal(v, field.value)
            end
        end
        if changed then
            node = checked_call(state, node, "ASDL_REFINEMENT", asdl.make, typename(node), tag, fields, node)
        end
        state.current = previous
        state.depth = state.depth - 1
        return node
    end

    function controller.run(options, node, ...)
        assert(type(options) == "table", "expected execution options")
        assert(not session(), "logistics run is already active")
        assert(isnode(node), "logistics requires a live AST node")
        assert(name_or_nil(options.phase), "invalid logistics phase")
        local phase, limit = options.phase or "default", options.depth_limit
        if limit == nil then limit = 96 end
        assert(math.type(limit) == "integer" and limit >= 1 and limit <= 256, "depth limit must be in 1..256")
        local context = {phase = phase, value = options.context}
        local state = {
            phase = phase, depth = 0, depth_limit = limit, current = node,
            context = context, writes = {}, origins = {},
            memo = {}, memo_entries = 0, memo_depth = 0,
        }
        local start = diagnostic.count("error") + diagnostic.count("fatal")
        local co = thread()
        sessions[co] = state
        local function execute(...)
            local result
            local input = node
            if options.refinements then input = refine_tree(state, input) end
            if not options.refinement_only then
                result = pack(dispatch(state, input, ...))
            else
                result = pack(input)
            end
            if diagnostic.count("error") + diagnostic.count("fatal") > start then
                return nil, "semantic diagnostics rejected the result"
            end
            if options.transform then
                if result[1] == nil then return nil, "transformation produced no root" end
                if not isnode(result[1]) or typename(result[1]) ~= typename(node) then
                    failure(state, input, "ASDL_TRANSFORM", "transformation must return a live root of the same type")
                end
                record_origin(result[1], input)
                if options.refinements and not rawequal(result[1], input) then result[1] = refine_tree(state, result[1]) end
            end
            return result
        end
        local ok, result, reason = pcall(execute, ...)
        sessions[co] = nil
        if not ok then
            if not state.failure then
                local err = type(result) == "string" and result or ("Lua raised a " .. type(result))
                -- Capture the protected-call error before reporting it to the host.
                pcall(failure, state, state.current, "ASDL_LOGISTICS", err)
            end
            local f = state.failure
            emit({severity = f.severity, code = f.code, message = f.message, location = f.location})
            return nil, f.message
        end
        if not result then return nil, reason end
        for target, writes in pairs(state.writes) do
            for k, v in pairs(writes) do
                if v == absent then annotate(target, k, nil) else annotate(target, k, v) end
            end
        end
        for target, origin in pairs(state.origins) do origins[target] = origin end
        return unpack(result, 1, result.n)
    end

    function asdl.logistics(node, ...)
        local state = session()
        if state then return dispatch(state, node, ...) end
        return controller.run({}, node, ...)
    end

    asdl.run_logistics = controller.run

    function asdl.refine(node, options)
        assert(not session(), "asdl.refine requires a standalone execution")
        return controller.run({
            refinements = true, refinement_only = true,
            phase = options and options.phase, depth_limit = options and options.depth_limit,
        }, node)
    end

    return controller
end
