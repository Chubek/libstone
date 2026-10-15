module gateml.tests.reference;
import gateml.Interface;
import J = gateml.serde.JSON;
import C = gateml.serde.CBOR;
import B = gateml.serde.BSON;
import M = gateml.serde.MSGPack;
import T = gateml.serde.TOML;
import Y = gateml.serde.YAML;
import F = gateml.serde.FlatBuffers;
import S = gateml.serde.SExpression;
import Wire = gateml.serde.Binary;
import gateml.ipc.Pipe;
import gateml.ipc.FIFO;
import gateml.ipc.PIDFile;
import gateml.ipc.SharedMemory;
import gateml.ipc.Semaphore;
import gateml.ipc.MessageQueue;
import gateml.ipc.Socket;
import std.exception : assertThrown;
import std.file : mkdir, remove, rmdir, exists, write;
import std.path : buildPath;
import std.conv : to;
import std.string : indexOf;
import std.stdio : writeln;
import core.sys.posix.unistd : getpid, fork, _exit;
import core.sys.posix.sys.wait : waitpid, WIFEXITED, WEXITSTATUS;
import core.stdc.errno : errno, EINTR;

enum tiny = `frontend Tiny;
lexer { skip WS = "[ \t\r\n]+"; token NUMBER = "[0-9]+"; }
parser { start expression; rule expression = NUMBER => Integer; }
ast { node Integer; }
adapter { lower Integer = integer; }`;

void frontendTests(string bundles) {
    auto frontend = compile(tiny);
    assert(evaluate(frontend, parse(frontend, " 42\n")) == 42);
    auto limited = TargetSpec.init; limited.maxSourceBytes = 1;
    assertThrown!GateError(parse(frontend, "42", limited));
    limited = TargetSpec.init; limited.maxTokens = 1;
    assertThrown!GateError(parse(frontend, "1 2", limited));
    limited = TargetSpec.init; limited.maxInstructions = 1;
    assertThrown!GateError(parse(frontend, "42", limited));
    assert(parse(frontend, " 42\n").children[0].span == Span(1, 3, 1, 2));
    assert(J.descriptor(J.readDescriptor(J.decode(J.descriptor(frontend)))) == J.descriptor(frontend));
    LoweringHooks lowering;
    lowering["integer"] = (Tree node, ref Program program, uint arity) {
        assert(arity == 0); program.emit(Opcode.constant, 43);
    };
    assert((new VM).run(lower(frontend, parse(frontend, "42"), lowering)) == 43);
    assertThrown!GateError(compile(tiny ~ " profile { option x = \"a\"; option x = \"b\"; }"));
    assertThrown!GateError(compile(tiny ~ " frontend Other;"));
    assertThrown!GateError(compile(tiny ~ "\0"));
    assertThrown!GateError(compile("frontend T;\0lexer { token X = \"a\"; } parser { start x; rule x = X => N; } ast { node N; }\xff"));
    assertThrown!GateError(compile(tiny ~ " // \xff"));
    assertThrown!GateError(parse(frontend, "\xff"));
    assert(location("λX", 2, 3) == Span(2, 3, 1, 3));
    // Tracked reader positions must agree with a full rescan of the source.
    auto multiLine = "frontend Positions;\nlexer {\n    skip WS = \"[ \\t\\r\\n]+\";\n"
        ~ "    token NUMBER = \"[0-9]+\";\n}\nparser {\n    start expression;\n"
        ~ "    rule expression = NUMBER => Integer;\n}\nast { node Integer; }\n"
        ~ "adapter { lower Integer = integer; }\n";
    assert(compile(multiLine).name == "Positions");
    foreach (i, char c; multiLine) if (c == ';' && i > multiLine.length / 2) {
        auto corrupted = multiLine[0 .. i] ~ "@" ~ multiLine[i + 1 .. $];
        try { compile(corrupted); assert(0, "expected failure"); }
        catch (GateError error) { assert(error.span == location(corrupted, i, i + 1), corrupted); }
    }
    try { compile(`frontend T; lexer { token X = "a"; } parser { start x; rule x = X => N; } ast { node N; } profile { option k = "unterminated`); assert(0, "expected string failure"); }
    catch (GateError error) { assert(error.span.line == 1 && error.span.column > 60); }
    enum rawNewline = "frontend T;\nlexer {\n    token X = \"a\n\";\n}\nparser { start x; rule x = X => N; }\nast { node N; }";
    try { compile(rawNewline); assert(0, "expected newline failure"); }
    catch (GateError error) {
        auto at = rawNewline[0 .. $].indexOf("\n\";"); // the newline inside the literal
        assert(error.span == location(rawNewline, at, at + 1), "newline span");
    }
    enum badEscape = `frontend T; lexer { token X = "a\q"; } parser { start x; rule x = X => N; } ast { node N; }`;
    try { compile(badEscape); assert(0, "expected escape failure"); }
    catch (GateError error) {
        auto at = badEscape[0 .. $].indexOf(`\q`) + 1; // report the escape character itself
        assert(error.span == location(badEscape, at, at + 1), "escape span");
    }
    assertThrown!GateError(compile(`frontend Bad; lexer { token X = "a*"; } parser { start x; rule x = X => N; } ast { node N; }`));
    assertThrown!GateError(compile(`frontend Bad; lexer { token X = "["; } parser { start x; rule x = X => N; } ast { node N; }`));
    assertThrown!GateError(compile(`frontend Bad; lexer { token X = "a"; } parser { start x; rule x = missing => N; } ast { node N; }`));
    assertThrown!GateError(compile(`frontend Bad; lexer { token X = "a"; } parser { start x; rule x = x => N; } ast { node N; }`));
    assertThrown!GateError(lower(frontend, parse(frontend, "9223372036854775808")));
    auto ambiguous = compile(`frontend Ambiguous; lexer { token X = "a"; }
        parser { start x; rule x = x x => Pair | X => Atom; } ast { node Pair; node Atom; }`);
    assertThrown!GateError(parse(ambiguous, "aaa"));
    auto empty = compile(`frontend Empty; lexer { token X = "a"; }
        parser { start x; rule x = => Empty; } ast { node Empty; }`);
    assert(parse(empty, "").name == "Empty");
    auto epsilon = compile(`frontend Nullable; lexer { token X = "a"; }
        parser { start s; rule s = e X e => Root; rule e = => Empty; }
        ast { node Root; node Empty; }`);
    assert(parse(epsilon, "a").children.length == 3);
    auto overlap = compile(`frontend Overlap; lexer { token WORD = "a|ab"; token SHORT = "a"; }
        parser { start x; rule x = WORD => Word; } ast { node Word; }`);
    assert(parse(overlap, "ab").children[0].text == "ab");
    assert(parse(overlap, "a").children[0].name == "WORD");
    auto lazyFrontend = compile(`frontend Lazy; lexer { token WORD = "a+?"; }
        parser { start x; rule x = WORD => Word; } ast { node Word; }`);
    assert(parse(lazyFrontend, "aaa").children[0].text == "aaa");
    auto multiline = compile(`frontend Multiline; lexer { token X = "(?m)a"; }
        parser { start x; rule x = X => Word; } ast { node Word; }`);
    assertThrown!GateError(parse(multiline, "\na")); // a later line is not a prefix
    string bomb = `frontend Bomb; lexer { token X = "a"; }
        parser { start n25; rule n0 = => Empty;`;
    foreach (i; 1 .. 26) bomb ~= " rule n" ~ i.to!string ~ " = n" ~ (i - 1).to!string ~ " n" ~ (i - 1).to!string ~ " => Pair;";
    bomb ~= " } ast { node Empty; node Pair; }";
    auto expansion = compile(bomb);
    assertThrown!GateError(parse(expansion, "")); // bounded before expanding 2^25 leaves
    limited = TargetSpec.init; limited.maxTreeDepth = 10;
    assertThrown!GateError(parse(expansion, "", limited));
    expansion.start = "n0"; // unused expansive rules must not consume parser resources
    assert(parse(expansion, "").name == "Empty");
    auto delimited = compile(`frontend Delimited;
        lexer { token X = "a"; token L = "\\("; token R = "\\)"; }
        parser { start s; rule s = L e R => Root; rule e = X => Atom; }
        ast { node Root; node Atom; }`);
    try { parse(delimited, "a"); assert(0, "expected opening delimiter failure"); }
    catch (GateError error) { assert(error.span.begin == 0); }
    auto metadata = compile(tiny ~ ` profile { option visit_Integer = "host::integer"; }`);
    assert(VisitorSpec.fromFrontend(metadata).handlers["Integer"] == "host::integer");
    foreach (bundle; ["C", "C++", "Delphi", "Java", "Python", "Ruby", "Rust", "Typescript", "Scheme"]) {
        auto definition = load(buildPath(bundles, bundle));
        auto sample = bundle == "Scheme" ? "(+ 1 (* 2 3))" : "1 + 2 * 3";
        auto tree = parse(definition, sample);
        assert(evaluate(definition, tree) == 7, bundle);
        auto program = lower(definition, tree);
        assert((new VM).run(decode(encode(program))) == 7);
        assert(J.descriptor(J.readDescriptor(J.frontendValue(definition))) == J.descriptor(definition));
        assert(grammar(definition).length);
        auto spec = VisitorSpec.fromFrontend(definition);
        assert(spec.handlers.length == definition.nodes.length);
        string[] events; VisitHandler[string] handlers;
        handlers["integer"] = (Tree node) { events ~= node.children[0].text; };
        handlers["mul"] = (Tree node) { events ~= "mul"; };
        handlers["add"] = (Tree node) { events ~= "add"; };
        visit(tree, spec, handlers);
        assert(events == ["1", "2", "3", "mul", "add"]);
        auto changed = rewrite(tree, [(Tree node) {
            if (node.terminal && node.text == "2") return new Tree(node.name, node.span, null, true, "4");
            return cast(Tree) null;
        }]);
        assert(evaluate(definition, changed) == 13);
        assert(evaluate(definition, tree) == 7); // original is untouched
    }
    auto c = load(buildPath(bundles, "C"));
    assert(evaluate(c, parse(c, "10-3-2")) == 5);
    assert(evaluate(c, parse(c, "/* hi */\n-123")) == -123);
    assertThrown!GateError(parse(c, "1+\n@"));
    try { parse(c, "1 2"); assert(0, "expected syntax failure"); }
    catch (GateError error) { assert(error.span.begin == 2); }
    try { parse(c, "1+\n"); assert(0, "expected EOF failure"); }
    catch (GateError error) { assert(error.span.begin == 3 && error.span.line == 2); }
    assert(evaluate(c, parse(c, "/* λ */ 1")) == 1);
    auto ts = load(buildPath(bundles, "Typescript"));
    assert(evaluate(ts, parse(ts, "8 >>> 1 | 1")) == 5);
    assert(parse(ts, "$x = 2").name == "Assign");
    auto scheme = load(buildPath(bundles, "Scheme"));
    assert(evaluate(scheme, parse(scheme, "(- -12) ; hi\n")) == 12);
    assert(evaluate(scheme, parse(scheme, "(quotient -10 3)")) == -3);
    assert(evaluate(scheme, parse(scheme, "(remainder -10 3)")) == -1);
    assertThrown!GateError(parse(scheme, "(+ 1 2 3)"));
    assertThrown!GateError(parse(scheme, "(1)"));
    auto external = parse(scheme, "(f 9)");
    Facility[string] facilities;
    facilities["namedcall"] = (const(long)[] args) { assert(args == [9L]); return args[0] + 1; };
    assertThrown!GateError(evaluate(scheme, external));
    assert(evaluate(scheme, external, facilities) == 10);
}

void treeTests() {
    auto leaf = new Tree("INTEGER", Span.init, null, true, "1");
    auto dag = new Tree("Pair", Span.init, [leaf, leaf]);
    auto copy = dag.clone();
    assert(copy.children[0] !is copy.children[1]);
    auto cyclic = new Tree("Pass"); cyclic.children = [cyclic];
    assertThrown!GateError(cyclic.clone());
    assertThrown!GateError(walk(cyclic, null));
    assertThrown!GateError(rewrite(cyclic, null));
    assertThrown!GateError(lower(compile(tiny), cyclic));
    auto broken = new Tree("Pass", Span.init, [cast(Tree) null]);
    assertThrown!GateError(broken.clone());
    assertThrown!GateError(walk(broken, null));
    assertThrown!GateError(rewrite(leaf, [(Tree node) { return cyclic; }]));
    assertThrown!GateError(validateTree(dag, 2));
    assertThrown!GateError(validateTree(dag, 10, 1));
}

void bundleTests(string directory) {
    auto missing = buildPath(directory, "missing.gml");
    assertThrown!GateError(load(missing));
    auto path = buildPath(directory, "bundle"); mkdir(path);
    scope(exit) {
        foreach (file; ["MANIFEST", "Profile.gml", "Lexer.gml"]) {
            auto filename = buildPath(path, file); if (exists(filename)) remove(filename);
        }
        rmdir(path);
    }
    assertThrown!GateError(load(path));
    write(buildPath(path, "MANIFEST"), "Profile.gml\n");
    try { load(path); assert(0, "expected missing fragment failure"); }
    catch (GateError error) { assert(error.source == buildPath(path, "Profile.gml")); }
    write(buildPath(path, "Profile.gml"), "frontend Broken;\n");
    write(buildPath(path, "Lexer.gml"), "lexer { token X = \"a\" }\n");
    write(buildPath(path, "MANIFEST"), "Profile.gml\r\nLexer.gml\r\n");
    try { load(path); assert(0, "expected fragment syntax failure"); }
    catch (GateError error) { assert(error.source == buildPath(path, "Lexer.gml") && error.span.line == 1); }
    write(buildPath(path, "MANIFEST"), "# first\r\nProfile.gml\r\nProfile.gml\r\n");
    try { load(path); assert(0, "expected duplicate manifest failure"); }
    catch (GateError error) { assert(error.source == buildPath(path, "MANIFEST") && error.span.line == 3); }
    write(buildPath(path, "MANIFEST"), "../missing.gml\n");
    assertThrown!GateError(load(path));
    write(buildPath(path, "MANIFEST"), "missing\0.gml\n");
    assertThrown!GateError(load(path));
}

void vmTests() {
    Program bad; bad.emit(Opcode.add); bad.emit(Opcode.ret);
    assertThrown!GateError(verify(bad));
    bad.instructions.length = 0; bad.emit(Opcode.constant); bad.emit(Opcode.constant); bad.emit(Opcode.ret);
    assertThrown!GateError(verify(bad));
    Program program; program.emit(Opcode.constant, long.max); program.emit(Opcode.constant, 1);
    program.emit(Opcode.add); program.emit(Opcode.ret);
    assert((new VM).run(program) == long.min);
    auto bytes = encode(program);
    foreach (i; 0 .. bytes.length) assertThrown!GateError(decode(bytes[0 .. i]));
    assertThrown!GateError(decode(bytes ~ cast(ubyte) 0));
    program.instructions[0].immediate = 1; program.instructions[1].immediate = 0;
    program.instructions[2].opcode = Opcode.sdiv;
    assertThrown!GateError((new VM).run(program));
    program.instructions[0].immediate = long.min; program.instructions[1].immediate = -1;
    assertThrown!GateError((new VM).run(program));
    program.instructions[2].opcode = Opcode.shl;
    assertThrown!GateError((new VM).run(program));
    program.instructions[1].immediate = 64;
    assertThrown!GateError((new VM).run(program));
    program.instructions[1].immediate = 1; program.instructions[2].opcode = Opcode.lshr;
    assert((new VM).run(program) == 0x4000000000000000L);
    auto vm = new VM;
    Program callback;
    callback.emit(Opcode.constant, 10); callback.emit(Opcode.constant, 20);
    callback.emit(Opcode.call, 0, 1, "nested"); callback.emit(Opcode.add); callback.emit(Opcode.ret);
    vm.facilities["nested"] = (const(long)[] args) {
        assertThrown!GateError(vm.run(program));
        assertThrown!GateError(vm.reset());
        // Mutation of the original program must not change the verified snapshot.
        callback.instructions[3].opcode = Opcode.mul;
        return args[0] + 1;
    };
    assert(vm.run(callback) == 31);
    vm.reset();
    long failHook(const(long)[] args) { throw new GateError("host failure"); }
    vm.facilities["nested"] = &failHook;
    assertThrown!GateError(vm.run(callback));
    assert(vm.run(program) == 0x4000000000000000L); // failure resets the execution state
}

void serdeTests(string artifacts) {
    auto value = J.decode(`{"z":null,"a":[true,false,-12,9223372036854775807,1.25,"λ\n\u0000\u007f\u0080"],"empty":{}}`);
    auto expected = J.encode(value);
    assert(J.encode(J.decode(expected)) == expected);
    assert(J.encode(T.decode(T.encode(value))) == expected);
    assert(J.encode(Y.decode(Y.encode(value))) == expected);
    assert(J.encode(S.decode(S.encode(value))) == expected);
    struct Codec {
        ubyte[] function(const J.JSONValue) emit;
        J.JSONValue function(const(ubyte)[]) read;
    }
    foreach (codec; [Codec(&C.encode, &C.decode), Codec(&M.encode, &M.decode),
                    Codec(&B.encode, &B.decode), Codec(&F.encode, &F.decode)]) {
        auto bytes = codec.emit(value);
        assert(J.encode(codec.read(bytes)) == expected);
        // Every proper prefix must be rejected by its corresponding decoder.
        foreach (i; 0 .. bytes.length) assertThrown!GateError(codec.read(bytes[0 .. i]));
        // Malformed wire data must fail with GateError, never a bounds error,
        // UTF exception, crash, or excessive allocation. Some mutations remain
        // valid values; those are intentionally permitted.
        foreach (i; 0 .. bytes.length) {
            auto changed = bytes.dup; changed[i] = 0xff;
            try { codec.read(changed); } catch (GateError expectedFailure) {}
        }
        uint random = 0x12345678;
        foreach (length; 0 .. 64) foreach (sample; 0 .. 8) {
            auto malformed = new ubyte[length];
            foreach (ref octet; malformed) {
                random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                octet = cast(ubyte) random;
            }
            try { codec.read(malformed); } catch (GateError expectedFailure) {}
        }
    }
    assert(C.encode(J.JSONValue(24)) == [cast(ubyte) 0x18, 0x18]);
    assert(C.encode(J.JSONValue(-1)) == [cast(ubyte) 0x20]);
    assert(M.encode(J.JSONValue("hi")) == [cast(ubyte) 0xa2, 0x68, 0x69]);
    assert(M.decode([cast(ubyte) 0xd1, 0xff, 0xfe]).integer == -2);
    assert(C.decode([cast(ubyte) 0xf9, 0x3e, 0x00]).floating == 1.5);
    assert(C.decode([cast(ubyte) 0xfa, 0x3f, 0xc0, 0x00, 0x00]).floating == 1.5);
    assertThrown!GateError(C.decode([cast(ubyte) 0xf9, 0x7c, 0x00]));
    assert(C.decode([cast(ubyte) 0x1b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]).uinteger == ulong.max);
    assertThrown!GateError(C.decode([cast(ubyte) 0x9f, 0xff]));
    assertThrown!GateError(M.decode([cast(ubyte) 0xc1]));
    assertThrown!GateError(B.encode(J.JSONValue(ulong.max)));
    auto unsignedObject = J.objectValue(); unsignedObject["n"] = J.JSONValue(ulong.max);
    assertThrown!GateError(B.encode(unsignedObject));
    assert(J.encode(C.decode(C.encode(unsignedObject))) == J.encode(unsignedObject));
    assert(J.encode(M.decode(M.encode(unsignedObject))) == J.encode(unsignedObject));
    auto large = J.objectValue(); auto array = J.arrayValue();
    foreach (i; 0 .. 40) { large[i.to!string] = J.JSONValue(cast(long) i - 33); array.array ~= J.JSONValue(cast(long) i); }
    large["array"] = array; large["min"] = J.JSONValue(long.min);
    large["string"] = J.JSONValue("λ".repeatForTest);
    assert(J.encode(M.decode(M.encode(large))) == J.encode(large));
    assert(J.encode(C.decode(C.encode(large))) == J.encode(large));
    assert(J.encode(B.decode(B.encode(large))) == J.encode(large));
    assert(C.encode(J.JSONValue(long.min)) == [cast(ubyte) 0x3b, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]);
    assertThrown!GateError(C.decode([cast(ubyte) 0xa2, 0x61, 'x', 1, 0x61, 'x', 2]));
    assertThrown!GateError(M.decode([cast(ubyte) 0x82, 0xa1, 'x', 1, 0xa1, 'x', 2]));
    assertThrown!GateError(C.decode([cast(ubyte) 0x61, 0xff]));
    assertThrown!GateError(M.decode([cast(ubyte) 0xa1, 0xff]));
    assertThrown!GateError(J.decode("\"\xff\""));
    assertThrown!GateError(C.encode(J.JSONValue("\xff")));
    assertThrown!GateError(M.encode(J.JSONValue("\xff")));
    assertThrown!GateError(J.decode("[" ~ "[".repeatForTest));
    assertThrown!GateError(S.decode("(object (\"x\" null) (\"x\" true))"));
    assertThrown!GateError(T.decode("x = 1"));
    assertThrown!GateError(T.decode("gateml_json = \"\x7f\""));
    foreach (invalid; ["[1,]", "{\"x\":1,}", "True", "00", "+1", "true junk", "\"\0\"", "\"\\ud800\""])
        assertThrown!GateError(J.decode(invalid));
    assertThrown!GateError(Y.decode("---true"));
    assert(S.decode("(real 18446744073709551615)").type == J.JSONType.float_);
    // Fixtures emitted by the standard Python FlatBuffers builder, including
    // trailing alignment padding and a preceding vtable at byte offset 10.
    auto external = cast(const(ubyte)[]) x"10000000474d4c4600000600080004000600000004000000040000006e756c6c00000000";
    assert(F.decode(external).type == J.JSONType.null_);
    auto followingVtable = cast(const(ubyte)[]) x"08000000474d4c46f8ffffff0c0000000600080004000000040000006e756c6c00000000";
    assert(F.decode(followingVtable).type == J.JSONType.null_);
    auto malformed = external.dup; malformed[0] = 17;
    assertThrown!GateError(F.decode(malformed)); // unaligned root
    Wire.Reader reader = Wire.Reader([cast(ubyte) 0]); reader.offset = 2;
    assertThrown!GateError(reader.take(1));
    reader.offset = 0; assertThrown!GateError(reader.le(9));
    Wire.Writer writer; assertThrown!GateError(writer.be(1, 0));
    write(buildPath(artifacts, "value.json"), expected);
    write(buildPath(artifacts, "value.toml"), T.encode(value));
    write(buildPath(artifacts, "value.cbor"), C.encode(value));
    write(buildPath(artifacts, "value.msgpack"), M.encode(value));
    write(buildPath(artifacts, "value.bson"), B.encode(value));
    write(buildPath(artifacts, "value.flatbuffer"), F.encode(value));
}

// Deep input checks do not need to construct a valid full JSON document.
private string repeatForTest(string text) {
    string result; foreach (i; 0 .. 130) result ~= text; return result;
}

void ipcTests(string directory) {
    auto pipe = new Pipe; scope(exit) pipe.close();
    pipe.writer.writeAll([cast(ubyte) 1, 0, 2]);
    ubyte[3] data; pipe.reader.readExact(data[]); assert(data[] == [1, 0, 2]);
    pipe.writer.close(); assert(pipe.reader.readSome(data[]) == 0);
    auto pair = Socket.pair(); scope(exit) { pair[0].close(); pair[1].close(); }
    pair[0].sendMessage([cast(ubyte) 3, 0, 4]); assert(pair[1].receiveMessage() == [3, 0, 4]);
    pair[0].sendMessage(null); assert(pair[1].receiveMessage().length == 0);
    auto socketPath = buildPath(directory, "socket");
    auto listener = new SocketListener(socketPath); scope(exit) listener.close();
    assertThrown!GateError(new SocketListener(socketPath));
    auto client = Socket.connect(socketPath); scope(exit) client.close();
    auto server = listener.accept(); scope(exit) server.close();
    client.sendMessage([cast(ubyte) 9]); assert(server.receiveMessage() == [9]);
    server.close(); assertThrown!GateError(client.sendMessage([cast(ubyte) 9])); // must not raise SIGPIPE
    auto fifo = new FIFO(buildPath(directory, "fifo")); scope(exit) fifo.close();
    auto reader = fifo.openReader(); scope(exit) reader.close();
    auto writer = fifo.openWriter(); scope(exit) writer.close();
    assertThrown!GateError(PIDFile.readPID(buildPath(directory, "fifo")));
    writer.writeAll([cast(ubyte) 7]); ubyte[1] byteBuffer; reader.readExact(byteBuffer[]); assert(byteBuffer[0] == 7);
    auto pidPath = buildPath(directory, "pid");
    auto pid = new PIDFile(pidPath); scope(exit) { pid.close(); remove(pidPath); }
    assert(PIDFile.readPID(pidPath) == getpid());
    assertThrown!GateError(new PIDFile(pidPath));
    pid.close(); auto replacement = new PIDFile(pidPath); replacement.close();
    auto name = "/gateml-test-" ~ getpid().to!string;
    auto memory = new SharedMemory(name ~ "-mem", 16); scope(exit) memory.close();
    assertThrown!GateError(new SharedMemory(name ~ "-mem", 16));
    assertThrown!GateError(new SharedMemory(name ~ "-mem", 17, false));
    auto semaphore = new Semaphore(name ~ "-sem"); scope(exit) semaphore.close();
    assertThrown!GateError(new Semaphore(name ~ "-sem"));
    assert(!semaphore.tryWait()); semaphore.post(); assert(semaphore.tryWait());
    auto queue = new MessageQueue(name ~ "-queue"); scope(exit) queue.close();
    assertThrown!GateError(new MessageQueue(name ~ "-queue"));
    assertThrown!GateError(queue.receive()); // nonblocking empty queue
    queue.send([cast(ubyte) 1, 2], 3);
    auto child = fork();
    assert(child >= 0);
    if (!child) {
        try {
            // Closing inherited handles must not unlink the parent's names.
            memory.close(); semaphore.close(); queue.close(); fifo.close(); listener.close();
            auto attached = new SharedMemory(name ~ "-mem", 16, false);
            auto sem = new Semaphore(name ~ "-sem", 0, false);
            auto mq = new MessageQueue(name ~ "-queue", false);
            auto message = mq.receive();
            if (message.bytes != [1, 2] || message.priority != 3) _exit(2);
            attached.bytes()[0] = 42; sem.post(); mq.send([cast(ubyte) 42], 4);
            mq.close(); sem.close(); attached.close(); _exit(0);
        } catch (Throwable error) { _exit(1); }
    }
    int status;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) assert(0, "waitpid failed");
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    auto retainedFIFO = new FIFO(buildPath(directory, "fifo"), false); retainedFIFO.close();
    auto retainedClient = Socket.connect(socketPath); scope(exit) retainedClient.close();
    auto retainedServer = listener.accept(); scope(exit) retainedServer.close();
    assert(semaphore.tryWait()); assert(memory.bytes()[0] == 42);
    auto message = queue.receive(); assert(message.bytes == [42] && message.priority == 4);
    queue.send(null); assert(queue.receive().bytes.length == 0);
    queue.close(); assertThrown!GateError(queue.receive());
    memory.close(); assertThrown!GateError(memory.bytes());
    semaphore.close(); assertThrown!GateError(semaphore.post());
    write(pidPath, "not a pid\n"); assertThrown!GateError(PIDFile.readPID(pidPath));
    write(pidPath, "1".repeatForTest); assertThrown!GateError(PIDFile.readPID(pidPath));
}

void pathLifetimeTests(string directory) {
    import std.file : getcwd, chdir, rename;
    import std.path : absolutePath;
    directory = absolutePath(directory);
    auto original = getcwd();
    auto first = buildPath(directory, "cwd-first");
    auto second = buildPath(directory, "cwd-second");
    mkdir(first); mkdir(second);
    scope(exit) {
        chdir(original);
        foreach (base; [first, second]) {
            foreach (name; ["fifo", "socket", "old-fifo", "old-socket"]) {
                auto filename = buildPath(base, name); if (exists(filename)) remove(filename);
            }
            rmdir(base);
        }
    }
    chdir(first);
    auto fifo = new FIFO("fifo"); scope(exit) fifo.close();
    auto listener = new SocketListener("socket"); scope(exit) listener.close();
    chdir(second);
    write("fifo", "sentinel"); write("socket", "sentinel");
    fifo.close(); listener.close();
    assert(!exists(buildPath(first, "fifo")) && !exists(buildPath(first, "socket")));
    assert(exists(buildPath(second, "fifo")) && exists(buildPath(second, "socket")));
    chdir(first);
    auto replacedFIFO = new FIFO("fifo"); scope(exit) replacedFIFO.close();
    auto replacedSocket = new SocketListener("socket"); scope(exit) replacedSocket.close();
    rename("fifo", "old-fifo"); write("fifo", "replacement");
    rename("socket", "old-socket"); write("socket", "replacement");
    replacedFIFO.close(); replacedSocket.close();
    assert(exists("fifo") && exists("socket")); // never unlink replacement files
}

void main(string[] args) {
    assert(args.length == 3 || args.length == 4, "reference-tests BUNDLE_DIR BUILD_DIR [ARTIFACT_DIR]");
    auto directory = buildPath(args[2], "reference-" ~ getpid().to!string);
    mkdir(directory);
    scope(exit) {
        foreach (file; ["value.json", "value.toml", "value.cbor", "value.msgpack", "value.bson", "value.flatbuffer"])
            if (exists(buildPath(directory, file))) remove(buildPath(directory, file));
        rmdir(directory);
    }
    frontendTests(args[1]); treeTests(); bundleTests(directory); vmTests();
    serdeTests(args.length == 4 ? args[3] : directory);
    ipcTests(directory);
    pathLifetimeTests(directory);
    writeln("GateML D reference/frontend/VM/serde/IPC tests passed");
}
