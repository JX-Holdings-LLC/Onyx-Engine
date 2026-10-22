#include "check.h"

#include <cstring>

int main(int argc, char ** argv) {
    const char * only = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    int failed = 0;
    for (const auto & t : onyx_test::registry()) {
        if (only && strcmp(only, t.suite) != 0) {
            continue;
        }
        const int before = onyx_test::failures();
        t.fn();
        run++;
        const bool ok = onyx_test::failures() == before;
        failed += !ok;
        printf("%s - %s.%s\n", ok ? "ok  " : "FAIL", t.suite, t.name);
    }
    printf("\n%d tests, %d failed\n", run, failed);
    if (run == 0) {
        fprintf(stderr, "no tests matched '%s'\n", only ? only : "");
        return 1;
    }
    return failed == 0 ? 0 : 1;
}
