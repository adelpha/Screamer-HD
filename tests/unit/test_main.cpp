#include "testing.h"
#include "debug_report.h"

#include <cstring>
#include <exception>

int main(int argc, char** argv) {
    install_debug_report(true);
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0, failed = 0, skipped = 0;
    for (const auto& c : testing::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        const int before = testing::state().failures;
        ++ran;
        try {
            c.fn();
        } catch (const testing::RequireFailed&) {
        } catch (const testing::Skip& s) {
            ++skipped;
            std::printf("[ SKIP ] %s (%s)\n", c.name, s.reason.c_str());
            continue;
        } catch (const std::exception& e) {
            testing::report(__FILE__, __LINE__, e.what());
        }
        const bool ok = testing::state().failures == before;
        if (!ok) ++failed;
        std::printf("[%s] %s\n", ok ? "  OK  " : " FAIL ", c.name);
    }
    std::printf("\n%d tests, %d failed, %d skipped\n", ran, failed, skipped);
    return failed ? 1 : 0;
}
