#include "etf_client.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <unordered_map>

// ── URL parsing ───────────────────────────────────────────────────────────────

ETFClient::ETFClient(const std::string& base_url,
                     const std::string& team_name,
                     const std::string& password)
    : team_name_(team_name), password_(password)
{
    parse_base_url(base_url);
}

// Parses "http://host:port" into host_ and port_.
// Also handles "http://host" (defaults port to "80").
void ETFClient::parse_base_url(const std::string& base_url) {
    std::string url = base_url;

    // Strip "http://"
    const std::string prefix = "http://";
    if (url.rfind(prefix, 0) == 0) {
        url = url.substr(prefix.size());
    }

    size_t colon = url.find(':');
    if (colon != std::string::npos) {
        host_ = url.substr(0, colon);
        port_ = url.substr(colon + 1);
    } else {
        host_ = url;
        port_ = "80";
    }
}

// ── Base64 encoder ────────────────────────────────────────────────────────────
// Needed for the "Authorization: Basic <base64(team:password)>" header.
// Standard RFC 4648 alphabet.

std::string ETFClient::base64_encode(const std::string& input) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    int val = 0, valb = -6;
    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(table[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

// ── Raw HTTP over POSIX socket ────────────────────────────────────────────────
// Opens a fresh TCP connection for every request.
// Returns the full raw HTTP response (status line + headers + body),
// or an empty string if the connection or send failed.

std::string ETFClient::send_http_request(const std::string& request) {
    // Resolve hostname to IP
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host_.c_str(), port_.c_str(), &hints, &res) != 0) {
        std::cerr << "[ETFClient] getaddrinfo failed for "
                  << host_ << ":" << port_ << "\n";
        return "";
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        std::cerr << "[ETFClient] socket() failed\n";
        return "";
    }

    if (::connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        close(sock);
        std::cerr << "[ETFClient] connect() failed: " << strerror(errno) << "\n";
        return "";
    }
    freeaddrinfo(res);

    // Send the full HTTP request string
    ssize_t sent = send(sock, request.c_str(), request.size(), 0);
    if (sent < 0) {
        close(sock);
        std::cerr << "[ETFClient] send() failed\n";
        return "";
    }

    // Read the response in chunks until the server closes the connection
    std::string response;
    char buf[4096];
    while (true) {
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;      // 0 = connection closed, -1 = error
        response.append(buf, n);
    }

    close(sock);
    return response;
}

// ── Strip HTTP headers ────────────────────────────────────────────────────────
// The server returns "HTTP/1.1 200 OK\r\n..headers..\r\n\r\n{body}".
// We only want the JSON body.

std::string ETFClient::extract_body(const std::string& response) {
    // Headers and body are separated by a blank line: \r\n\r\n
    const std::string sep = "\r\n\r\n";
    size_t pos = response.find(sep);
    if (pos == std::string::npos) return response;  // no headers found, return as-is
    return response.substr(pos + sep.size());
}

// ── Public API ────────────────────────────────────────────────────────────────

ETFResult ETFClient::create(int32_t amount) {
    return post_amount("/create", amount);
}

ETFResult ETFClient::redeem(int32_t amount) {
    return post_amount("/redeem", amount);
}

bool ETFClient::health_check() {
    std::string request =
        "GET /health HTTP/1.0\r\n"
        "Host: " + host_ + ":" + port_ + "\r\n"
        "Connection: close\r\n"
        "\r\n";

    std::string response = send_http_request(request);
    if (response.empty()) return false;

    // Healthy if HTTP 200 and body contains "ok"
    return response.find("200") != std::string::npos &&
           response.find("\"ok\"") != std::string::npos;
}

// ── Internal: build and send POST request ────────────────────────────────────

ETFResult ETFClient::post_amount(const std::string& endpoint, int32_t amount) {
    std::string body    = "{\"amount\":" + std::to_string(amount) + "}";
    std::string auth    = base64_encode(team_name_ + ":" + password_);

    // Build a valid HTTP/1.0 POST request.
    // HTTP/1.0 is intentional — the server closes the connection after the
    // response, which signals end-of-body without needing chunked parsing.
    std::string request =
        "POST " + endpoint + " HTTP/1.0\r\n"
        "Host: "           + host_ + ":" + port_ + "\r\n"
        "Authorization: Basic " + auth + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" +
        body;

    std::string response = send_http_request(request);

    if (response.empty()) {
        return {false, "No response from ETF service", -1};
    }

    // Check HTTP status code (first line: "HTTP/1.0 200 OK")
    bool http_ok = (response.find("HTTP/1.0 200") != std::string::npos ||
                    response.find("HTTP/1.1 200") != std::string::npos);

    std::string json_body = extract_body(response);

    if (json_body.empty()) {
        return {false, "Empty response body", -1};
    }

    return parse_response(json_body, http_ok);
}

// ── Minimal JSON parser ───────────────────────────────────────────────────────
// Handles both server response shapes without any library:
//   {"success":true,  "message":"Created 5 UNDY...", "undy_balance":10}
//   {"success":false, "message":"Insufficient...",    "undy_balance": 5}

ETFResult ETFClient::parse_response(const std::string& body, bool http_ok) {
    if (!http_ok) {
        return {false, "HTTP error: " + body, -1};
    }
    ETFResult result{false, body, -1};  // safe defaults

    // ── success ──────────────────────────────────────────────────────────────
    if (body.find("\"success\":true") != std::string::npos) {
        result.success = true;
    } else if (body.find("\"success\":false") != std::string::npos) {
        result.success = false;
    } else {
        result.message = "Unparseable response: " + body;
        return result;
    }

    // ── message ───────────────────────────────────────────────────────────────
    {
        const std::string key = "\"message\":\"";
        size_t start = body.find(key);
        if (start != std::string::npos) {
            start += key.size();
            size_t end = body.find('"', start);
            if (end != std::string::npos) {
                result.message = body.substr(start, end - start);
            }
        }
    }

    // ── undy_balance ──────────────────────────────────────────────────────────
    {
        const std::string key = "\"undy_balance\":";
        size_t pos = body.find(key);
        if (pos != std::string::npos) {
            pos += key.size();
            while (pos < body.size() && body[pos] == ' ') ++pos;
            try {
                result.undy_balance = std::stoi(body.substr(pos));
            } catch (...) {
                result.undy_balance = -1;   // non-fatal, balance unknown
            }
        }
    }

    return result;
}

// Add to etf_client.cpp:
std::unordered_map<std::string, int32_t> ETFClient::get_positions(int client_id) {
    std::string request =
        "GET /positions/" + std::to_string(client_id) + " HTTP/1.0\r\n"
        "Host: " + host_ + ":" + port_ + "\r\n"
        "Connection: close\r\n"
        "\r\n";

    std::string response = send_http_request(request);
    std::string body     = extract_body(response);

    // Parse {"client_id":8,"positions":{"GOLD":7,"BLUE":4}}
    std::unordered_map<std::string, int32_t> positions;
    size_t pos = body.find("\"positions\":");
    if (pos == std::string::npos) return positions;

    // Find each "TICKER":value pair
    size_t start = body.find('{', pos + 12);
    size_t end   = body.find('}', start);
    if (start == std::string::npos || end == std::string::npos) return positions;

    std::string pos_block = body.substr(start + 1, end - start - 1);
    size_t i = 0;
    while (i < pos_block.size()) {
        // Find ticker
        size_t q1 = pos_block.find('"', i);
        if (q1 == std::string::npos) break;
        size_t q2 = pos_block.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string ticker = pos_block.substr(q1 + 1, q2 - q1 - 1);

        // Find value
        size_t colon = pos_block.find(':', q2);
        if (colon == std::string::npos) break;
        int32_t value = std::stoi(pos_block.substr(colon + 1));
        positions[ticker] = value;
        i = pos_block.find(',', colon);
        if (i == std::string::npos) break;
        ++i;
    }
    return positions;
}