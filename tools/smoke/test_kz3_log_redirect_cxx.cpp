// compile check: the redirect macros survive C++ headers that include <cstdio> (libstdc++'s <cstdio> #undefs
// printf & co.; kz3_log.h includes it first so a later include is a no-op). Built by test_kz3_log.sh; exits 0 when
// printf routed through kz3_log_printf (checked via the return value: a filtered line returns 0, real printf > 0).
#include <string>
#include <cstdio>
#include <vector>
int main()
{
    std::vector<std::string> v{"x"};
    int filtered = printf("[mfc-list] cxx %s\n", v[0].c_str());   // routed + filtered -> 0; real printf -> 15
    return filtered == 0 ? 0 : 1;
}
