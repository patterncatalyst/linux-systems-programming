// contracts_demo -- C++26 Contracts (P2900), built four times, once per
// evaluation semantic, for ch49's gate E.
//
// GCC 16 implements P2900 behind -fcontracts, and selects the semantic with
// -fcontract-evaluation-semantic=[ignore|observe|enforce|quick_enforce].
// The same source, the same violation, four different observable behaviors --
// which is the whole point of the proposal: whether a contract is checked is
// a build-time policy, not a property of the code.
//
// `x` is declared const because P2900 requires a value parameter named in a
// postcondition to be const: the postcondition talks about the value the
// caller passed, so the function must not have reassigned it in between.

#include <cstdio>

int scaled(const int x) pre(x > 0) post(r: r > x) { return x * 2; }

int main() {
    std::printf("contracts: scaled(21) = %d\n", scaled(21));
    std::printf("contracts: violating the precondition now...\n");
    std::fflush(stdout);
    std::printf("contracts: scaled(-1) = %d\n", scaled(-1));
    std::printf("contracts: still running after the violation\n");
    return 0;
}
