/* Callee for the by-value aggregate interoperability suite (LP64). */
struct pair { long a; long b; };
struct mixed { long tag; double value; };
struct big { long v[5]; };

struct pair make_pair(long a, long b) { struct pair p; p.a = a; p.b = b; return p; }
long sum_pair(struct pair p) { return p.a + p.b; }

struct mixed make_mixed(long tag, double value)
{ struct mixed m; m.tag = tag; m.value = value; return m; }
double scaled_mixed(struct mixed m) { return m.value * (double)m.tag; }

long sum_big(struct big b) { return b.v[0] + b.v[1] + b.v[2] + b.v[3] + b.v[4]; }
