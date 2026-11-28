/* Caller for the by-value aggregate interoperability suite (LP64). */
struct pair { long a; long b; };
struct mixed { long tag; double value; };
struct big { long v[5]; };

struct pair make_pair(long a, long b);
long sum_pair(struct pair p);
struct mixed make_mixed(long tag, double value);
double scaled_mixed(struct mixed m);
long sum_big(struct big b);

int main(void)
{
    struct pair p = make_pair(20L, 22L);
    if (p.a != 20L || p.b != 22L) return 1;
    if (sum_pair(p) != 42L) return 2;

    struct mixed m = make_mixed(3L, 1.5);
    if (m.tag != 3L) return 3;
    if (scaled_mixed(m) != 4.5) return 4;

    struct big b = {{1L, 2L, 3L, 4L, 5L}};
    if (sum_big(b) != 15L) return 5;
    return 0;
}
