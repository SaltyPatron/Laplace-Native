/* Links the installed package and composes two IDs through its public header only. */
#include <laplace/laplace.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    lp_id a, b, ab, ab2;
    lp_id_codepoint('a', &a);
    lp_id_codepoint('b', &b);
    lp_id ch[2] = { a, b };
    lp_id_compose(ch, 2, &ab);
    lp_id_compose(ch, 2, &ab2);
    if (memcmp(&ab, &ab2, sizeof ab) != 0 || memcmp(&ab, &a, sizeof ab) == 0) { puts("compose"); return 1; }
    puts("ok");
    return 0;
}
