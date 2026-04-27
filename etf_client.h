#pragma once

#include <string>
#include <cstdint>

// Returned by every mutating ETF call (create / redeem).
// On success:  success=true,  message describes the operation,
//              undy_balance reflects your new UNDY position.
// On failure:  success=false, message contains the server error string
//              (e.g. "Insufficient positions: KNAN: have 3, need 5"),
//              undy_balance reflects your current UNDY position unchanged.
// On network error: success=false, undy_balance=-1
struct ETFResult {
    bool        success;
    std::string message;
    int32_t     undy_balance;
};

// Thin synchronous wrapper around the NDFEX ETF REST API.
// Uses raw POSIX sockets — no external dependencies required.
//
// Endpoints used:
//   POST /create  — exchange N lots of every dorm underlying for N UNDY
//   POST /redeem  — exchange N UNDY for N lots of every dorm underlying
//   GET  /health  — liveness probe
//
// Authentication: HTTP Basic Auth (same credentials as the matching engine).
//
// Thread safety: NOT thread-safe. Guard with an external mutex if needed.
class ETFClient {
public:
    // base_url  : e.g. "http://129.74.160.245:5000"  (no trailing slash)
    // team_name : your team login, e.g. "group8"
    // password  : your team password
    ETFClient(const std::string& base_url,
              const std::string& team_name,
              const std::string& password);

    // Exchange `amount` lots of every dorm underlying for `amount` UNDY.
    // Prerequisite: hold >= amount of EACH of the 10 dorm underlyings.
    // Atomic server-side — no partial fill risk.
    ETFResult create(int32_t amount);

    // Exchange `amount` UNDY for `amount` lots of every dorm underlying.
    // Prerequisite: hold >= amount UNDY.
    ETFResult redeem(int32_t amount);

    // GET /health — returns true if service is up.
    // Call once at bot startup to verify connectivity.
    bool health_check();

private:
    std::string host_;      // e.g. "129.74.160.245"
    std::string port_;      // e.g. "5000"
    std::string team_name_;
    std::string password_;

    // Splits "http://host:port" into host_ and port_
    void parse_base_url(const std::string& base_url);

    // Sends a raw HTTP request string over a fresh TCP socket.
    // Returns the full HTTP response (headers + body), or "" on error.
    std::string send_http_request(const std::string& request);

    // Builds and sends POST {endpoint} {"amount": amount} with Basic Auth.
    ETFResult post_amount(const std::string& endpoint, int32_t amount);

    // Strips HTTP headers from a response, returns just the body.
    static std::string extract_body(const std::string& response);

    // Base64 encodes "team:password" for the Authorization header.
    static std::string base64_encode(const std::string& input);

    // Minimal JSON parser for the two API response shapes.
    static ETFResult parse_response(const std::string& body, bool http_ok);
};