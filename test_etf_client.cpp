#include "etf_client.h"
#include <iostream>

int main() {
    ETFClient etf("http://129.74.160.245:5000", "group8", "Uangjrty");

    // ── Test 1: Health check ─────────────────────────────────────────────────
    std::cout << "=== Health Check ===\n";
    bool healthy = etf.health_check();
    std::cout << "Service healthy: " << (healthy ? "YES" : "NO") << "\n\n";

    if (!healthy) {
        std::cerr << "Service unreachable — check IP/port. Stopping.\n";
        return 1;
    }

    // ── Test 2: Create 0 — should fail with "Amount must be positive" ────────
    std::cout << "=== Create 0 (expect failure) ===\n";
    ETFResult r1 = etf.create(0);
    std::cout << "success: "      << r1.success      << "\n";
    std::cout << "message: "      << r1.message      << "\n";
    std::cout << "undy_balance: " << r1.undy_balance << "\n\n";

    // ── Test 3: Redeem 0 — should fail with "Amount must be positive" ────────
    std::cout << "=== Redeem 0 (expect failure) ===\n";
    ETFResult r2 = etf.redeem(0);
    std::cout << "success: "      << r2.success      << "\n";
    std::cout << "message: "      << r2.message      << "\n";
    std::cout << "undy_balance: " << r2.undy_balance << "\n\n";

    // ── Test 4: Create 1 — will fail unless you hold all 10 underlyings ──────
    // This is still useful: it confirms auth works and tells you exactly
    // which underlying you're short, e.g. "Insufficient positions: KNAN: have 0, need 1"
    std::cout << "=== Create 1 (expect insufficient positions) ===\n";
    ETFResult r3 = etf.create(1);
    std::cout << "success: "      << r3.success      << "\n";
    std::cout << "message: "      << r3.message      << "\n";
    std::cout << "undy_balance: " << r3.undy_balance << "\n\n";

    return 0;
}