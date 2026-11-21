// Standalone acsem @VERSION@ interpreter, Java 8 or later. MIT-0.
public final class @CLASS@ {
    private static final java.lang.String[] MODEL = {
@MODEL@
    };
    private static final long MIN = -2147483648L, MAX = 2147483647L;
    private static final class @CLASS@Fault extends java.lang.RuntimeException {
        private static final long serialVersionUID = 1L;
        @CLASS@Fault(java.lang.String text) { super(text); }
    }
    private static void error(java.lang.String text) { throw new @CLASS@Fault("ACSEM_ERROR: " + text); }
    private static void limit(java.lang.String text) { throw new @CLASS@Fault("ACSEM_LIMIT: " + text); }
    private static final class @CLASS@Value {
        final int type;
        final long n;
        final java.lang.String text;
        @CLASS@Value(int type, long n, java.lang.String text) { this.type = type; this.n = n; this.text = text; }
    }
    private static @CLASS@Value value(int type, long n) { return new @CLASS@Value(type, n, null); }
    private static final class @CLASS@Constructor {
        java.lang.String name;
        int sort, rule;
        int[] fields;
    }
    private static final class @CLASS@Instruction {
        int op, x, y, n;
        java.lang.String text;
    }
    private static final class @CLASS@Term {
        int ctor;
        @CLASS@Value[] fields;
    }
    private static final class @CLASS@Token {
        final java.lang.String text;
        final int kind, at;
        @CLASS@Token(java.lang.String text, int kind, int at) { this.text = text; this.kind = kind; this.at = at; }
    }
    private static final class @CLASS@Outcome {
        final int status;
        final java.util.List<@CLASS@Value> out;
        final java.util.Map<java.lang.String, @CLASS@Value> bindings;
        @CLASS@Outcome(int status, java.util.List<@CLASS@Value> out, java.util.Map<java.lang.String, @CLASS@Value> bindings) {
            if (out.size() > 64) limit("transient result capacity");
            if (bindings.size() > 64) limit("binding result capacity");
            this.status = status; this.out = out; this.bindings = bindings;
        }
    }
    private static @CLASS@Outcome empty(int status) {
        return new @CLASS@Outcome(status, new java.util.ArrayList<@CLASS@Value>(), new java.util.LinkedHashMap<java.lang.String, @CLASS@Value>());
    }
    private static @CLASS@Outcome output(int status, @CLASS@Value value) {
        return new @CLASS@Outcome(status, java.util.Collections.singletonList(value), new java.util.LinkedHashMap<java.lang.String, @CLASS@Value>());
    }
    private final byte[] source;
    private final java.util.List<@CLASS@Token> tokens = new java.util.ArrayList<>();
    private final java.util.List<@CLASS@Term> terms = new java.util.ArrayList<>();
    private final java.util.List<@CLASS@Value> cells = new java.util.ArrayList<>();
    private final java.util.Set<java.lang.String> symbols = new java.util.HashSet<>();
    private final @CLASS@Constructor[] ctors;
    private final @CLASS@Instruction[] code;
    private final int start;
    private int pos, steps;

    private @CLASS@(byte[] source) {
        if (source.length > 65536) limit("source exceeds 64 KiB");
        this.source = source;
        java.lang.String[] head = MODEL[0].split(" ");
        start = java.lang.Integer.parseInt(head[0]);
        ctors = new @CLASS@Constructor[java.lang.Integer.parseInt(head[1])];
        code = new @CLASS@Instruction[java.lang.Integer.parseInt(head[2])];
        for (int i = 0; i < ctors.length; ++i) {
            java.lang.String[] row = MODEL[i + 1].split(" ");
            @CLASS@Constructor c = new @CLASS@Constructor();
            c.name = row[0]; c.sort = java.lang.Integer.parseInt(row[1]); c.rule = java.lang.Integer.parseInt(row[3]);
            c.fields = new int[java.lang.Integer.parseInt(row[2])];
            for (int j = 0; j < c.fields.length; ++j) c.fields[j] = java.lang.Integer.parseInt(row[j + 4]);
            ctors[i] = c;
        }
        for (int i = 0; i < code.length; ++i) {
            java.lang.String[] row = MODEL[ctors.length + i + 1].split(" ");
            @CLASS@Instruction a = new @CLASS@Instruction();
            a.op = java.lang.Integer.parseInt(row[0]); a.x = java.lang.Integer.parseInt(row[1]);
            a.y = java.lang.Integer.parseInt(row[2]); a.n = java.lang.Integer.parseInt(row[3]); a.text = row[4]; code[i] = a;
        }
        lex();
    }
    private void syntax(java.lang.String message, int at) {
        int line = 1, col = 1;
        for (int i = 0; i < at; ++i) { if (source[i] == '\n') { ++line; col = 1; } else ++col; }
        error(message + " at " + line + ":" + col);
    }
    private void syntax(java.lang.String message) { syntax(message, tokens.get(pos).at); }
    private static boolean letter(int c) { return c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c == '_'; }
    private static boolean digit(int c) { return c >= '0' && c <= '9'; }
    private void lex() {
        int at = 0;
        while (at < source.length) {
            int c = source[at] & 255, begin = at, kind = 3;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++at; continue; }
            if (letter(c)) {
                kind = 1; ++at;
                while (at < source.length && (letter(source[at]) || digit(source[at]))) ++at;
            } else if (digit(c)) {
                kind = 2; ++at;
                while (at < source.length && digit(source[at])) ++at;
            } else if ("{}(),;:=|+*/-<".indexOf(c) >= 0) {
                ++at;
                if (c == '=' && at < source.length && source[at] == '=') ++at;
            } else syntax(c == 0 ? "NUL in source" : "invalid term syntax", at);
            if (at - begin > 64) limit("source token exceeds 64 bytes");
            if (tokens.size() == 20000) limit("too many tokens");
            tokens.add(new @CLASS@Token(new java.lang.String(source, begin, at - begin, java.nio.charset.StandardCharsets.US_ASCII), kind, begin));
        }
        tokens.add(new @CLASS@Token("<eof>", 0, source.length));
    }
    private boolean eat(java.lang.String text) {
        if (!tokens.get(pos).text.equals(text)) return false;
        ++pos; return true;
    }
    private void expect(java.lang.String text) { if (!eat(text)) syntax("expected '" + text + "'"); }
    private java.lang.String ident() {
        if (tokens.get(pos).kind != 1) syntax("expected identifier");
        return tokens.get(pos++).text;
    }
    private @CLASS@Value symbol(java.lang.String text) {
        symbols.add(text);
        if (symbols.size() > 256) limit("runtime symbol limit");
        return new @CLASS@Value(2, 0, text);
    }
    private int parse(int sort, int depth) {
        if (depth > 128) limit("parse depth exceeded");
        if (terms.size() == 2048) limit("program syntax node limit");
        int index = terms.size();
        @CLASS@Term term = new @CLASS@Term(); terms.add(term);
        java.lang.String name = ident();
        int ci = 0;
        while (ci < ctors.length && !ctors[ci].name.equals(name)) ++ci;
        if (ci == ctors.length) syntax("unknown constructor");
        @CLASS@Constructor c = ctors[ci];
        if (c.sort != sort) syntax("constructor has wrong sort");
        term.ctor = ci; term.fields = new @CLASS@Value[c.fields.length]; expect("(");
        for (int i = 0; i < c.fields.length; ++i) {
            if (i != 0) expect(",");
            if (c.fields[i] == -1) {
                boolean neg = eat("-");
                if (tokens.get(pos).kind != 2) syntax("integer field expected");
                java.lang.String text = (neg ? "-" : "") + tokens.get(pos++).text;
                long n = 0;
                try { n = java.lang.Long.parseLong(text); }
                catch (java.lang.NumberFormatException ex) { syntax("integer field outside 32-bit range"); }
                if (n < MIN || n > MAX) syntax("integer field outside 32-bit range");
                term.fields[i] = value(1, n);
            } else if (c.fields[i] == -2) term.fields[i] = symbol(ident());
            else term.fields[i] = value(4, parse(c.fields[i], depth + 1));
        }
        expect(")"); return index;
    }
    private void tick(int depth) {
        if (++steps > 100000) limit("evaluation step budget exceeded");
        if (depth > 128) limit("evaluation nesting exceeded");
    }
    private @CLASS@Value expr(int pc, int term, java.util.List<@CLASS@Value> inputs, java.util.Map<java.lang.String, @CLASS@Value> env, int depth) {
        tick(depth);
        @CLASS@Instruction e = code[pc];
        if (e.op == 1) return value(1, e.n);
        if (e.op == 2) return symbol(e.text);
        if (e.op == 3) return terms.get(term).fields[e.n];
        if (e.op == 4) return e.n < inputs.size() ? inputs.get(e.n) : null;
        @CLASS@Value a = expr(e.x, term, inputs, env, depth + 1);
        if (e.op == 5) return a != null && a.type == 2 ? env.get(a.text) : null;
        if (e.op == 6) return a != null && a.type == 3 && a.n >= 0 && a.n < cells.size() ? cells.get((int)a.n) : null;
        @CLASS@Value b = expr(e.y, term, inputs, env, depth + 1);
        if (a == null || b == null || a.type != 1 || b.type != 1) return null;
        long n;
        switch (e.op) {
            case 7: n = a.n + b.n; break;
            case 8: n = a.n - b.n; break;
            case 9: n = a.n * b.n; break;
            case 10:
                if (b.n == 0 || a.n == MIN && b.n == -1) return null;
                n = a.n / b.n; break;
            case 11: n = a.n == b.n ? 1 : 0; break;
            case 12: n = a.n < b.n ? 1 : 0; break;
            default: throw new @CLASS@Fault("ACSEM_ERROR: invalid expression instruction");
        }
        return n < MIN || n > MAX ? null : value(1, n);
    }
    private @CLASS@Outcome action(int pc, int term, java.util.List<@CLASS@Value> inputs, java.util.Map<java.lang.String, @CLASS@Value> env, int depth) {
        tick(depth);
        @CLASS@Instruction a = code[pc];
        if (a.op == 13) return empty(0);
        if (a.op == 14) return empty(1);
        if (a.op >= 15 && a.op <= 17) {
            @CLASS@Value v = expr(a.x, term, inputs, env, depth + 1);
            if (v == null) return empty(1);
            if (a.op == 16) return empty(v.type == 1 && v.n != 0 ? 0 : 1);
            return output(a.op == 17 ? 2 : 0, v);
        }
        if (a.op == 19) {
            if (cells.size() == 1024) limit("allocated cell limit");
            int cell = cells.size(); cells.add(null); return output(0, value(3, cell));
        }
        if (a.op == 18 || a.op == 20) {
            @CLASS@Value v = expr(a.x, term, inputs, env, depth + 1), w = expr(a.y, term, inputs, env, depth + 1);
            if (a.op == 18) {
                if (v == null || v.type != 2 || w == null) return empty(1);
                @CLASS@Outcome r = empty(0); r.bindings.put(v.text, w); return r;
            }
            if (v == null || v.type != 1 || w == null || w.type != 3 || w.n < 0 || w.n >= cells.size()) return empty(1);
            cells.set((int)w.n, v); return empty(0);
        }
        if (a.op == 21) return node((int)terms.get(term).fields[a.n].n, inputs, env, depth + 1);
        @CLASS@Outcome left = action(a.x, term, inputs, env, depth + 1);
        if (a.op == 22) {
            if (left.status != 0) return left;
            java.util.Map<java.lang.String, @CLASS@Value> merged = new java.util.LinkedHashMap<>(env);
            merged.putAll(left.bindings); return new @CLASS@Outcome(0, left.out, merged);
        }
        if (a.op == 26) return left.status == 1 ? action(a.y, term, inputs, env, depth + 1) : left;
        if (a.op == 27) return left.status == 2 ? action(a.y, term, left.out, env, depth + 1) : left;
        if (left.status != 0) return left;
        @CLASS@Outcome right = action(a.y, term, a.op == 23 ? left.out : inputs, a.op == 25 ? left.bindings : env, depth + 1);
        if (right.status != 0) return right;
        java.util.Map<java.lang.String, @CLASS@Value> bindings = new java.util.LinkedHashMap<>();
        if (a.op != 25) {
            for (java.lang.String key : right.bindings.keySet()) if (left.bindings.containsKey(key)) return empty(1);
            bindings.putAll(left.bindings);
        }
        bindings.putAll(right.bindings);
        java.util.List<@CLASS@Value> out = new java.util.ArrayList<>();
        if (a.op != 23) out.addAll(left.out);
        out.addAll(right.out); return new @CLASS@Outcome(0, out, bindings);
    }
    private @CLASS@Outcome node(int term, java.util.List<@CLASS@Value> inputs, java.util.Map<java.lang.String, @CLASS@Value> env, int depth) {
        tick(depth);
        return action(ctors[terms.get(term).ctor].rule, term, inputs, env, depth + 1);
    }
    private static byte[] readSource(java.io.InputStream input) throws java.io.IOException {
        java.io.ByteArrayOutputStream buffer = new java.io.ByteArrayOutputStream();
        byte[] block = new byte[4096];
        while (buffer.size() <= 65536) {
            int n = input.read(block, 0, java.lang.Math.min(block.length, 65537 - buffer.size()));
            if (n < 0) break;
            buffer.write(block, 0, n);
        }
        if (buffer.size() > 65536) limit("input exceeds 64 KiB");
        return buffer.toByteArray();
    }
    private static int cli(java.lang.String[] args) throws java.io.IOException {
        java.lang.String term = null, path = null, output = null;
        for (int i = 0; i < args.length; ++i) {
            int eq = args[i].indexOf('=');
            java.lang.String flag = eq < 0 ? args[i] : args[i].substring(0, eq);
            if (eq < 0 && (flag.equals("--help") || flag.equals("-h"))) {
                java.lang.System.out.println("generated interpreter [--term PROGRAM | --term-file PATH] [-o FILE]\nExit: completed=0, failed/escaped=1, errors=2."); return 0;
            }
            if (eq < 0 && flag.equals("--version")) { java.lang.System.out.println("acsem @VERSION@"); return 0; }
            if (eq < 0 && flag.equals("--run")) continue;
            if (!flag.equals("--term") && !flag.equals("--term-file") && !flag.equals("-o") && !flag.equals("--output")) error("invalid option");
            java.lang.String arg;
            if (eq >= 0) arg = args[i].substring(eq + 1);
            else { if (++i == args.length) error("missing option value"); arg = args[i]; }
            if (flag.equals("--term")) term = arg;
            else if (flag.equals("--term-file")) path = arg;
            else output = arg;
        }
        if (term != null && path != null) error("use only one of --term and --term-file");
        byte[] source;
        if (term != null) source = term.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        else if (path != null) { try (java.io.InputStream input = new java.io.FileInputStream(path)) { source = readSource(input); } }
        else source = readSource(java.lang.System.in);
        @CLASS@ rt = new @CLASS@(source);
        int root = rt.parse(rt.start, 1);
        if (rt.tokens.get(rt.pos).kind != 0) rt.syntax("trailing term input");
        @CLASS@Outcome r = rt.node(root, new java.util.ArrayList<@CLASS@Value>(), new java.util.LinkedHashMap<java.lang.String, @CLASS@Value>(), 1);
        java.lang.StringBuilder text = new java.lang.StringBuilder(new java.lang.String[]{"completed", "failed", "escaped"}[r.status]);
        for (@CLASS@Value v : r.out) {
            text.append(' ');
            if (v.type == 2) text.append("token:").append(v.text);
            else { if (v.type == 3) text.append("cell:"); text.append(v.n); }
        }
        text.append('\n');
        byte[] bytes = text.toString().getBytes(java.nio.charset.StandardCharsets.US_ASCII);
        if (output != null && !output.equals("-")) {
            try (java.io.OutputStream stream = new java.io.FileOutputStream(output)) { stream.write(bytes); }
        } else {
            java.lang.System.out.write(bytes); java.lang.System.out.flush();
            if (java.lang.System.out.checkError()) error("output write failed");
        }
        return r.status == 0 ? 0 : 1;
    }
    public static void main(java.lang.String[] args) {
        try { java.lang.System.exit(cli(args)); }
        catch (@CLASS@Fault ex) { java.lang.System.err.println(ex.getMessage()); java.lang.System.exit(2); }
        catch (java.io.IOException ex) { java.lang.System.err.println("ACSEM_ERROR: " + ex.getMessage()); java.lang.System.exit(2); }
    }
}
