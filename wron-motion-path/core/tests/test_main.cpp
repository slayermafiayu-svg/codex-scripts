#include <cstring>

#include "test.h"

namespace wmptest {
std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
int& failures() {
    static int f = 0;
    return f;
}
const char*& currentCase() {
    static const char* c = "";
    return c;
}
}  // namespace wmptest

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failedCases = 0;
    for (const auto& c : wmptest::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        wmptest::currentCase() = c.name;
        const int before = wmptest::failures();
        c.fn();
        ++run;
        const bool ok = wmptest::failures() == before;
        if (!ok) ++failedCases;
        std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", c.name);
    }
    std::printf("\n%d test cases, %d failed, %d failed checks\n", run, failedCases, wmptest::failures());
    return wmptest::failures() == 0 ? 0 : 1;
}
