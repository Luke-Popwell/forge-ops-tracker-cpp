/*
 * A minimal, hand-rolled test runner rather than an external framework (Catch2/GoogleTest):
 * mirrors sdks/c/tests/test_forgeops_tracker.c's own decision on purpose, not by accident. Two
 * reasons that decision carries over to C++ specifically: this SDK is closest in spirit and
 * toolchain to sdks/c (same libcurl/execinfo/POSIX-signal foundations, same "one platform-shaped
 * client, not a portable-everywhere one" scope), and a test binary is the one place in this repo
 * where reaching for a vendored dependency purely for the tests themselves, on top of the library's
 * own real runtime dependencies (libcurl, nlohmann_json: see the README's "Dependencies"
 * section), would add a second dependency for no capability this file's own macros don't already
 * provide. C++ exceptions make the hand-rolled approach noticeably less painful than C's version:
 * RUN() below catches whatever a test throws instead of every ASSERT needing its own manual
 * unwind-and-return dance: so there wasn't a real capability gap pulling toward Catch2 either.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "forge_ops_tracker/client.hpp"
#include "forge_ops_tracker/configuration.hpp"
#include "forge_ops_tracker/delivery_queue.hpp"
#include "forge_ops_tracker/event_builder.hpp"
#include "forge_ops_tracker/forge_ops_tracker.hpp"
#include "forge_ops_tracker/metric_buffer.hpp"
#include "forge_ops_tracker/histogram_bucketer.hpp"
#include "forge_ops_tracker/performance_flusher.hpp"
#include "forge_ops_tracker/pii_scrubber.hpp"
#include "forge_ops_tracker/reporter.hpp"
#include "forge_ops_tracker/span_buffer.hpp"
#include "forge_ops_tracker/trace_parent.hpp"

using forge_ops_tracker::Client;
using forge_ops_tracker::Configuration;
using forge_ops_tracker::DeliveryQueue;
using forge_ops_tracker::EventBuilder;
using forge_ops_tracker::Reporter;

/* ---- test harness ----------------------------------------------------------------------------- */

static int g_tests_run = 0;
static int g_tests_failed = 0;
static bool g_current_test_failed = false;

#define TEST(name) static void name()
#define RUN(name)                                                              \
    do {                                                                       \
        g_tests_run++;                                                        \
        g_current_test_failed = false;                                        \
        try {                                                                  \
            name();                                                           \
        } catch (const std::exception& e) {                                    \
            g_current_test_failed = true;                                     \
            std::printf("  uncaught exception: %s\n", e.what());              \
        } catch (...) {                                                       \
            g_current_test_failed = true;                                     \
            std::printf("  uncaught non-standard exception\n");               \
        }                                                                     \
        if (g_current_test_failed) {                                          \
            g_tests_failed++;                                                 \
            std::printf("FAIL %s\n", #name);                                  \
        } else {                                                              \
            std::printf("PASS %s\n", #name);                                  \
        }                                                                     \
    } while (0)

#define ASSERT_TRUE(cond)                                                                    \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::printf("  assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_current_test_failed = true;                                                    \
            return;                                                                          \
        }                                                                                    \
    } while (0)

/* ---- local HTTP test server --------------------------------------------------------------------
 * Same technique and same lesson learned as sdks/c/tests/test_forgeops_tracker.c's own
 * start_test_server(): the full request (headers *and* body) must be read before responding, or
 * closing the connection the moment the header terminator shows up can race curl still writing its
 * POST body: confirmed there as a real bug in that test file's first draft, applied here from the
 * start instead of rediscovering it. Runs its accept loop on a background std::thread rather than a
 * forked process (no fork() available on every target this repo would eventually port to, and
 * nothing here needs the process isolation forking buys); poll()s with a short timeout so the
 * thread notices `stopping_` promptly instead of blocking in accept() forever. */
class TestServer {
public:
    /* status_code >= 0: a normal server that accepts, reads, and responds. status_code < 0: a
     * "black hole": the listening socket exists (so a DSN can point at its port) but nothing
     * ever calls accept() on it, so a client's connection sits in the kernel backlog and its
     * request hangs until the client's own timeout gives up. Used to test DeliveryQueue's
     * bounded-drop behavior below without needing a real slow server. */
    explicit TestServer(int status_code) : status_code_(status_code) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int reuse = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(fd_, 16);

        socklen_t len = sizeof(addr);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);

        if (status_code_ >= 0) {
            worker_ = std::thread(&TestServer::run, this);
        }
    }

    ~TestServer() {
        stopping_ = true;
        if (worker_.joinable()) {
            worker_.join();
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    int port() const { return port_; }

    int request_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(requests_.size());
    }

    std::string last_request() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.empty() ? std::string() : requests_.back();
    }

    std::string request_at(int index) {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<std::size_t>(index) < requests_.size() ? requests_[static_cast<std::size_t>(index)] : std::string();
    }

    bool wait_for_request_count(int n, int timeout_ms = 2000) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (request_count() < n) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }

private:
    void run() {
        while (!stopping_) {
            pollfd pfd{fd_, POLLIN, 0};
            int rv = ::poll(&pfd, 1, 100);
            if (rv <= 0) {
                continue;
            }
            int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            handle_connection(client);
            ::close(client);
        }
    }

    void handle_connection(int client) {
        char buf[16384];
        std::size_t total = 0;
        char* header_end = nullptr;

        while (header_end == nullptr && total < sizeof(buf) - 1) {
            ssize_t n = ::read(client, buf + total, sizeof(buf) - total - 1);
            if (n <= 0) {
                break;
            }
            total += static_cast<std::size_t>(n);
            buf[total] = '\0';
            header_end = std::strstr(buf, "\r\n\r\n");
        }

        long content_length = 0;
        if (header_end != nullptr) {
            const char* cl = std::strstr(buf, "Content-Length:");
            if (cl != nullptr) {
                content_length = std::strtol(cl + std::strlen("Content-Length:"), nullptr, 10);
            }
        }

        std::size_t body_already_read =
            header_end != nullptr ? total - static_cast<std::size_t>(header_end + 4 - buf) : 0;
        while (header_end != nullptr && static_cast<long>(body_already_read) < content_length &&
               total < sizeof(buf) - 1) {
            ssize_t n = ::read(client, buf + total, sizeof(buf) - total - 1);
            if (n <= 0) {
                break;
            }
            total += static_cast<std::size_t>(n);
            buf[total] = '\0';
            body_already_read += static_cast<std::size_t>(n);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.emplace_back(buf, total);
        }

        char response[256];
        int len = std::snprintf(response, sizeof(response),
                                 "HTTP/1.1 %d Status\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                                 status_code_);
        ::write(client, response, static_cast<std::size_t>(len));
    }

    int fd_ = -1;
    int port_ = 0;
    int status_code_;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    std::mutex mutex_;
    std::vector<std::string> requests_;
};

static std::string dsn_for(const TestServer& server, const std::string& api_key = "the-api-key") {
    return "http://" + api_key + "@127.0.0.1:" + std::to_string(server.port()) + "/api/v1/events";
}

/* Exceptions defined in their own namespace purely so event_builder tests below have a real,
 * predictable demangled name to assert against (verified directly: __cxa_demangle turns
 * "N13fot_test_exc9BoomErrorE" into "fot_test_exc::BoomError", no "class " prefix, no surprises:
 * checked against real backtrace_symbols()/typeid output on this machine before writing these
 * tests, not assumed from the header comment alone). */
namespace fot_test_exc {
class BoomError : public std::runtime_error {
public:
    explicit BoomError(const std::string& message) : std::runtime_error(message) {}
};
} // namespace fot_test_exc

/* ---- Configuration -------------------------------------------------------------------------- */

TEST(configuration_defaults) {
    Configuration config;
    ASSERT_TRUE(config.environment == "production");
    ASSERT_TRUE(config.scrub_pii == true);
    ASSERT_TRUE(config.capture_source_context == true);
    ASSERT_TRUE(config.queue_size == 1000);
    ASSERT_TRUE(config.timeout_seconds == 2);
    ASSERT_TRUE(config.enabled_environments.size() == 2);
    ASSERT_TRUE(config.enabled_environments.count("production") == 1);
    ASSERT_TRUE(config.enabled_environments.count("staging") == 1);
    ASSERT_TRUE(!config.is_enabled()); // no dsn set
}

TEST(configuration_api_key_and_ingestion_uri) {
    Configuration config;
    config.dsn = "https://abc123@forgeops.example/api/v1/events";

    ASSERT_TRUE(config.api_key().has_value());
    ASSERT_TRUE(*config.api_key() == "abc123");
    ASSERT_TRUE(config.ingestion_uri().has_value());
    ASSERT_TRUE(*config.ingestion_uri() == "https://forgeops.example/api/v1/events");
}

TEST(configuration_api_key_percent_decodes) {
    Configuration config;
    config.dsn = "https://ab%2Fc@forgeops.example/api/v1/events";
    ASSERT_TRUE(*config.api_key() == "ab/c");
}

TEST(configuration_empty_or_malformed_dsn) {
    Configuration config;

    config.dsn = "";
    ASSERT_TRUE(!config.api_key().has_value());
    ASSERT_TRUE(!config.ingestion_uri().has_value());

    config.dsn = "not-a-url";
    ASSERT_TRUE(!config.api_key().has_value());
    ASSERT_TRUE(!config.ingestion_uri().has_value());
}

TEST(configuration_dsn_with_no_userinfo) {
    Configuration config;
    config.dsn = "https://forgeops.example/no-userinfo";

    ASSERT_TRUE(!config.api_key().has_value());
    ASSERT_TRUE(*config.ingestion_uri() == "https://forgeops.example/no-userinfo");
}

TEST(configuration_is_enabled) {
    Configuration config;
    config.dsn = "https://key@host/path";

    config.environment = "production";
    ASSERT_TRUE(config.is_enabled());

    config.environment = "development"; // not in the default enabled_environments set
    ASSERT_TRUE(!config.is_enabled());

    config.environment = "production";
    config.dsn = std::nullopt;
    ASSERT_TRUE(!config.is_enabled());
}

TEST(configuration_log_invokes_logger_and_no_ops_without_one) {
    Configuration config;
    config.log("no logger set, must not crash"); // no logger assigned yet

    std::string captured;
    config.logger = [&captured](const std::string& message) { captured = message; };
    config.log("hello");
    ASSERT_TRUE(captured == "hello");
}

/* ---- PII scrubbing -------------------------------------------------------------------------- */

TEST(pii_scrub_email) {
    ASSERT_TRUE(forge_ops_tracker::pii_scrubber::scrub_string("contact user@example.com for help") ==
                "contact [EMAIL FILTERED] for help");
}

TEST(pii_scrub_ssn) {
    ASSERT_TRUE(forge_ops_tracker::pii_scrubber::scrub_string("ssn on file: 123-45-6789") ==
                "ssn on file: [SSN FILTERED]");
}

TEST(pii_scrub_credit_card) {
    std::string input = "charged card 4242-4242-4242-4242 successfully";
    ASSERT_TRUE(forge_ops_tracker::pii_scrubber::scrub_string(input) != input);
}

TEST(pii_leaves_ordinary_numeric_id_alone) {
    std::string input = "order id 1234567890123456";
    ASSERT_TRUE(forge_ops_tracker::pii_scrubber::scrub_string(input) == input);
}

TEST(pii_scrub_known_token_formats) {
    /* Each fake credential is split across adjacent string literals (concatenated at compile time,
     * same runtime value either way) rather than one contiguous literal: none of these were ever
     * real, but GitHub push protection flags the shape regardless of context. Same convention as
     * sdks/c's own test file. */
    std::vector<std::string> cases = {
        "Authorization: Bearer abc123DEF.456-xyz",
        std::string("aws key ") + "AKIA" + "ABCDEFGHIJKLMNOP" + " in use",
        std::string("stripe key ") + "sk_live_" + "abcdefghijklmnop",
        std::string("github token ") + "ghp_" + "abcdefghijklmnopqrstuvwxyz0123456789",
        "jwt eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.dQw4w9WgXcQ",
    };
    for (const auto& input : cases) {
        ASSERT_TRUE(forge_ops_tracker::pii_scrubber::scrub_string(input) != input);
    }
}

TEST(pii_sensitive_key_scrub_ignores_case_and_punctuation) {
    using forge_ops_tracker::pii_scrubber::kRedacted;
    ASSERT_TRUE(forge_ops_tracker::pii_scrubber::scrub("hello", std::string("API_KEY")) == kRedacted);
    ASSERT_TRUE(forge_ops_tracker::pii_scrubber::scrub("hello", std::string("Api-Key")) == kRedacted);
    ASSERT_TRUE(forge_ops_tracker::pii_scrubber::scrub("hello", std::string("apiKey")) == kRedacted);
    ASSERT_TRUE(forge_ops_tracker::pii_scrubber::scrub("hello", std::string("X-Api-Key")) == kRedacted);
    ASSERT_TRUE(forge_ops_tracker::pii_scrubber::scrub("hello", std::string("username")) == "hello");
    ASSERT_TRUE(forge_ops_tracker::pii_scrubber::scrub("hello", std::nullopt) == "hello");
}

TEST(pii_scrub_recurses_through_nested_objects_and_arrays) {
    nlohmann::json value = {
        {"user", {{"password", "shh-secret"}, {"note", "contact user@example.com"}}},
        {"items", nlohmann::json::array({{{"api_key", "shh-secret-2"}}})},
    };

    nlohmann::json scrubbed = forge_ops_tracker::pii_scrubber::scrub(value);

    ASSERT_TRUE(scrubbed["user"]["password"] == forge_ops_tracker::pii_scrubber::kRedacted);
    ASSERT_TRUE(scrubbed["user"]["note"] == "contact [EMAIL FILTERED]");
    ASSERT_TRUE(scrubbed["items"][0]["api_key"] == forge_ops_tracker::pii_scrubber::kRedacted);
}

TEST(pii_scrub_null_under_sensitive_key_stays_null) {
    nlohmann::json value = {{"password", nullptr}};
    nlohmann::json scrubbed = forge_ops_tracker::pii_scrubber::scrub(value);
    /* Deliberate behavior, not an oversight: pii_scrubber.cpp's own scrub() only redacts a
     * sensitive key when the value isn't already null: redacting "null" to "[FILTERED]" would
     * turn "this field was never set" into "this field held something we hid", a strictly worse
     * signal for debugging. */
    ASSERT_TRUE(scrubbed["password"].is_null());
}

/* ---- EventBuilder --------------------------------------------------------------------------- */

TEST(event_builder_basic_fields) {
    Configuration config;
    config.environment = "production";
    config.release = "abc123";
    config.server_name = "web-1";
    EventBuilder builder(config);

    nlohmann::json payload;
    try {
        throw fot_test_exc::BoomError("boom");
    } catch (const std::exception& e) {
        payload = builder.build(e, {{"order_id", "42"}});
    }

    ASSERT_TRUE(payload["exception_class"] == "fot_test_exc::BoomError");
    ASSERT_TRUE(payload["message"] == "boom");
    ASSERT_TRUE(payload["environment"] == "production");
    ASSERT_TRUE(payload["release"] == "abc123");
    ASSERT_TRUE(payload["server_name"] == "web-1");
    ASSERT_TRUE(payload["context"]["order_id"] == "42");
    ASSERT_TRUE(payload["sdk_name"] == "cpp");
    ASSERT_TRUE(payload["backtrace"].is_array());
    ASSERT_TRUE(!payload["backtrace"].empty());

    static const std::regex kIso8601(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$)");
    ASSERT_TRUE(std::regex_match(payload["occurred_at"].get<std::string>(), kIso8601));
}

TEST(event_builder_release_and_server_name_are_null_when_unset) {
    Configuration config; // release/server_name left at their std::nullopt defaults
    EventBuilder builder(config);

    nlohmann::json payload;
    try {
        throw fot_test_exc::BoomError("boom");
    } catch (const std::exception& e) {
        payload = builder.build(e);
    }

    ASSERT_TRUE(payload["release"].is_null());
    ASSERT_TRUE(payload["server_name"].is_null());
}

TEST(event_builder_scrubs_message_and_context_by_default) {
    Configuration config; // scrub_pii defaults to true
    EventBuilder builder(config);

    nlohmann::json payload;
    try {
        throw fot_test_exc::BoomError("failed for user@example.com");
    } catch (const std::exception& e) {
        payload = builder.build(e, {{"api_key", "shh-secret"}});
    }

    ASSERT_TRUE(payload["message"] == "failed for [EMAIL FILTERED]");
    ASSERT_TRUE(payload["context"]["api_key"] == forge_ops_tracker::pii_scrubber::kRedacted);
    ASSERT_TRUE(payload.dump().find("shh-secret") == std::string::npos);
}

TEST(event_builder_leaves_payload_untouched_when_scrub_pii_disabled) {
    Configuration config;
    config.scrub_pii = false;
    EventBuilder builder(config);

    nlohmann::json payload;
    try {
        throw fot_test_exc::BoomError("contact user@example.com");
    } catch (const std::exception& e) {
        payload = builder.build(e);
    }

    ASSERT_TRUE(payload["message"] == "contact user@example.com");
}

TEST(event_builder_includes_the_user_when_given_one_never_scrubbed_even_though_its_an_email) {
    Configuration config;
    EventBuilder builder(config);

    nlohmann::json payload;
    try {
        throw fot_test_exc::BoomError("boom");
    } catch (const std::exception& e) {
        payload = builder.build(e, nlohmann::json::object(), {{"id", 42}, {"email", "ada@example.com"}});
    }

    ASSERT_TRUE(payload["user"]["email"] == "ada@example.com");
}

TEST(event_builder_includes_breadcrumbs_when_given) {
    Configuration config;
    EventBuilder builder(config);
    nlohmann::json crumbs = nlohmann::json::array({{{"category", "controller"}, {"message", "GET /orders/42"}, {"level", "info"}, {"timestamp", "2024-01-15T10:29:58Z"}, {"data", nlohmann::json::object()}}});

    nlohmann::json payload;
    try {
        throw fot_test_exc::BoomError("boom");
    } catch (const std::exception& e) {
        payload = builder.build(e, nlohmann::json::object(), nlohmann::json::object(), crumbs);
    }

    ASSERT_TRUE(payload["breadcrumbs"] == crumbs);
}

TEST(event_builder_omits_the_breadcrumbs_key_entirely_when_none_were_given) {
    Configuration config;
    EventBuilder builder(config);

    nlohmann::json payload;
    try {
        throw fot_test_exc::BoomError("boom");
    } catch (const std::exception& e) {
        payload = builder.build(e);
    }

    ASSERT_TRUE(!payload.contains("breadcrumbs"));
}

TEST(event_builder_scrubs_breadcrumb_message_and_data_but_not_category_level_or_timestamp) {
    Configuration config;
    EventBuilder builder(config);
    nlohmann::json crumbs = nlohmann::json::array({{{"category", "custom"}, {"message", "emailed alice@example.com"}, {"level", "info"}, {"timestamp", "2024-01-15T10:29:58Z"}, {"data", {{"email", "alice@example.com"}, {"password", "hunter2"}}}}});

    nlohmann::json payload;
    try {
        throw fot_test_exc::BoomError("boom");
    } catch (const std::exception& e) {
        payload = builder.build(e, nlohmann::json::object(), nlohmann::json::object(), crumbs);
    }

    const auto& crumb = payload["breadcrumbs"][0];
    ASSERT_TRUE(crumb["message"] == "emailed [EMAIL FILTERED]");
    ASSERT_TRUE(crumb["category"] == "custom");
    ASSERT_TRUE(crumb["level"] == "info");
    ASSERT_TRUE(crumb["timestamp"] == "2024-01-15T10:29:58Z");
    ASSERT_TRUE(crumb["data"]["email"] == "[EMAIL FILTERED]");
    ASSERT_TRUE(crumb["data"]["password"] == "[FILTERED]");
}

TEST(event_builder_omits_the_user_key_entirely_when_none_was_given) {
    Configuration config;
    EventBuilder builder(config);

    nlohmann::json payload;
    try {
        throw fot_test_exc::BoomError("boom");
    } catch (const std::exception& e) {
        payload = builder.build(e);
    }

    ASSERT_TRUE(!payload.contains("user"));
}

TEST(event_builder_non_std_exception_has_no_class_or_message) {
    Configuration config;
    EventBuilder builder(config);

    std::exception_ptr ptr;
    try {
        throw 42; // not a std::exception at all: legal in C++, and genuinely un-nameable
    } catch (...) {
        ptr = std::current_exception();
    }

    nlohmann::json payload = builder.build(ptr);
    ASSERT_TRUE(payload["exception_class"] == "unknown exception");
    ASSERT_TRUE(payload["message"] == "");
}

TEST(event_builder_backtrace_frames_are_well_formed_and_in_app_for_this_binary) {
    Configuration config;
    EventBuilder builder(config);

    nlohmann::json payload;
    try {
        throw fot_test_exc::BoomError("boom");
    } catch (const std::exception& e) {
        payload = builder.build(e);
    }

    bool saw_in_app_frame = false;
    for (const auto& frame : payload["backtrace"]) {
        ASSERT_TRUE(frame["file"].is_string());
        ASSERT_TRUE(frame["line"].is_null()); // no line-level info from backtrace_symbols(): see the header comment
        ASSERT_TRUE(frame["method"].is_string());
        ASSERT_TRUE(frame["in_app"].is_boolean());
        if (frame["in_app"] == true) {
            saw_in_app_frame = true;
        }
    }
    /* This test binary links forge_ops_tracker statically, so every frame that unwinds back into
     * this executable (at minimum the RUN() call site in main()) shares this binary's own image
     * name and should be classified in_app: confirmed directly against real backtrace_symbols()
     * output on this machine before relying on it (see the header's is_in_app() comment for the
     * caveat this only works because nothing here is a separate shared library). */
    ASSERT_TRUE(saw_in_app_frame);
}

TEST(event_builder_source_context_is_a_documented_noop) {
    /* capture_source_context defaults to true (see configuration_defaults above), but a frame here
     * never carries a real file+line pair to key a disk read off of (backtrace_symbols() only ever
     * gives an image name + symbol), so no frame should ever come back with context_line/
     * pre_context/post_context, whether the flag is left at its default or explicitly toggled. */
    Configuration config_enabled;
    config_enabled.capture_source_context = true;
    EventBuilder enabled_builder(config_enabled);

    Configuration config_disabled;
    config_disabled.capture_source_context = false;
    EventBuilder disabled_builder(config_disabled);

    for (EventBuilder* builder : {&enabled_builder, &disabled_builder}) {
        nlohmann::json payload;
        try {
            throw fot_test_exc::BoomError("boom");
        } catch (const std::exception& e) {
            payload = builder->build(e);
        }

        for (const auto& frame : payload["backtrace"]) {
            ASSERT_TRUE(frame.find("context_line") == frame.end());
            ASSERT_TRUE(frame.find("pre_context") == frame.end());
            ASSERT_TRUE(frame.find("post_context") == frame.end());
        }
    }
}

/* ---- Client (real local HTTP server) ------------------------------------------------------------ */

TEST(client_delivers_on_2xx_response) {
    TestServer server(202);
    Configuration config;
    config.dsn = dsn_for(server);
    Client client(config);

    ASSERT_TRUE(client.deliver({{"message", "boom"}}));
    ASSERT_TRUE(server.wait_for_request_count(1));
}

TEST(client_returns_false_on_non_2xx_response) {
    TestServer server(500);
    Configuration config;
    config.dsn = dsn_for(server);
    Client client(config);

    ASSERT_TRUE(!client.deliver({{"message", "boom"}}));
}

TEST(client_returns_false_with_no_dsn) {
    Configuration config; // no dsn
    Client client(config);
    ASSERT_TRUE(!client.deliver({{"message", "boom"}}));
}

TEST(client_returns_false_when_unreachable) {
    Configuration config;
    config.dsn = "http://key@127.0.0.1:1/api/v1/events"; // port 1: nothing listens there
    config.timeout_seconds = 1;
    Client client(config);
    ASSERT_TRUE(!client.deliver({{"message", "boom"}}));
}

TEST(client_sends_authorization_bearer_header_and_json_body) {
    TestServer server(202);
    Configuration config;
    config.dsn = dsn_for(server, "my-secret-key");
    Client client(config);

    ASSERT_TRUE(client.deliver({{"exception_class", "BoomError"}}));
    ASSERT_TRUE(server.wait_for_request_count(1));

    std::string request = server.last_request();
    ASSERT_TRUE(request.find("Authorization: Bearer my-secret-key") != std::string::npos);
    ASSERT_TRUE(request.find("Content-Type: application/json") != std::string::npos);
    ASSERT_TRUE(request.find("\"exception_class\":\"BoomError\"") != std::string::npos);
}

/* ---- DeliveryQueue -------------------------------------------------------------------------- */

TEST(delivery_queue_push_delivers_via_background_thread) {
    TestServer server(202);
    Configuration config;
    config.dsn = dsn_for(server);
    DeliveryQueue queue(config, Client(config));

    ASSERT_TRUE(queue.push({{"n", 1}}));
    ASSERT_TRUE(queue.push({{"n", 2}}));
    ASSERT_TRUE(server.wait_for_request_count(2));
}

TEST(delivery_queue_drops_once_full) {
    /* A black-hole server so the worker thread's first delivery blocks for the full
     * timeout_seconds, giving this test a deterministic window in which to fill the queue past
     * capacity: see TestServer's own header comment for why status_code < 0 hangs instead of
     * responding. */
    TestServer server(-1);
    Configuration config;
    config.dsn = dsn_for(server);
    config.queue_size = 2;
    config.timeout_seconds = 1;
    /* Guarded: the logger is called from this thread (the drop below) *and* from the queue's own
     * worker thread (the black-hole delivery timing out), a real data race ThreadSanitizer flagged
     * on the unsynchronized vector this test originally used. */
    std::mutex logged_mutex;
    std::vector<std::string> logged;
    config.logger = [&logged, &logged_mutex](const std::string& message) {
        std::lock_guard<std::mutex> lock(logged_mutex);
        logged.push_back(message);
    };

    DeliveryQueue queue(config, Client(config));

    ASSERT_TRUE(queue.push({{"n", 1}})); // picked up by the worker almost immediately, which then blocks on the black hole
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); // let the worker actually start blocking on it

    ASSERT_TRUE(queue.push({{"n", 2}})); // queue: [2]: 1 of 2 slots used
    ASSERT_TRUE(queue.push({{"n", 3}})); // queue: [2, 3]: at capacity
    ASSERT_TRUE(!queue.push({{"n", 4}})); // over capacity: dropped

    bool logged_drop = false;
    std::lock_guard<std::mutex> lock(logged_mutex);
    for (const auto& message : logged) {
        if (message.find("queue full") != std::string::npos) {
            logged_drop = true;
        }
    }
    ASSERT_TRUE(logged_drop);
}

TEST(delivery_queue_destructor_drains_pending_items_before_returning) {
    TestServer server(202);
    {
        Configuration config;
        config.dsn = dsn_for(server);
        DeliveryQueue queue(config, Client(config));

        queue.push({{"n", 1}});
        queue.push({{"n", 2}});
        queue.push({{"n", 3}});
        // queue destructs here: its own destructor sets stopping_ and joins the worker, and
        // run()'s own loop keeps draining whatever's left in the queue before actually exiting
        // (see delivery_queue.cpp's own run(): the wait predicate is satisfied immediately once
        // stopping_ is true, but the loop still pops and delivers until the queue is genuinely
        // empty, not just up to whatever was mid-flight).
    }
    ASSERT_TRUE(server.request_count() == 3);
}

/* ---- Reporter ------------------------------------------------------------------------------- */

TEST(reporter_report_does_nothing_when_disabled) {
    TestServer server(202);
    Configuration config;
    config.dsn = dsn_for(server);
    config.environment = "development"; // not in enabled_environments: disabled
    DeliveryQueue queue(config, Client(config));
    Reporter reporter(config, EventBuilder(config), queue);

    reporter.report(fot_test_exc::BoomError("boom"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(server.request_count() == 0);
}

TEST(reporter_report_delivers_when_enabled) {
    TestServer server(202);
    Configuration config;
    config.dsn = dsn_for(server);
    config.environment = "production";
    DeliveryQueue queue(config, Client(config));
    Reporter reporter(config, EventBuilder(config), queue);

    reporter.report(fot_test_exc::BoomError("boom"), {{"order_id", "1"}});
    ASSERT_TRUE(server.wait_for_request_count(1));
}

TEST(reporter_report_includes_the_given_user_never_scrubbed_even_though_its_an_email) {
    TestServer server(202);
    Configuration config;
    config.dsn = dsn_for(server);
    config.environment = "production";
    DeliveryQueue queue(config, Client(config));
    Reporter reporter(config, EventBuilder(config), queue);

    reporter.report(fot_test_exc::BoomError("boom"), nlohmann::json::object(), {{"id", 42}, {"email", "alice@example.com"}});

    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(server.last_request().find("alice@example.com") != std::string::npos);
}

TEST(reporter_report_includes_the_given_breadcrumbs_in_the_delivered_payload) {
    TestServer server(202);
    Configuration config;
    config.dsn = dsn_for(server);
    config.environment = "production";
    DeliveryQueue queue(config, Client(config));
    Reporter reporter(config, EventBuilder(config), queue);
    nlohmann::json crumbs = nlohmann::json::array({{{"category", "controller"}, {"message", "GET /orders/42"}, {"level", "info"}, {"timestamp", "2024-01-15T10:29:58Z"}, {"data", nlohmann::json::object()}}});

    reporter.report(fot_test_exc::BoomError("boom"), nlohmann::json::object(), nlohmann::json::object(), crumbs);

    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(server.last_request().find("GET /orders/42") != std::string::npos);
}

TEST(reporter_report_never_throws_for_a_non_std_exception) {
    Configuration config; // disabled (no dsn): this test only cares that report() itself never throws
    DeliveryQueue queue(config, Client(config));
    Reporter reporter(config, EventBuilder(config), queue);

    std::exception_ptr ptr;
    try {
        throw 42;
    } catch (...) {
        ptr = std::current_exception();
    }
    reporter.report(ptr); // must not throw: RUN() would catch and fail this test if it did
}

/* ---- forge_ops_tracker module-level facade --------------------------------------------------- */

TEST(tracker_init_and_capture_exception_deliver_through_the_full_stack) {
    TestServer server(202);
    forge_ops_tracker::reset_for_testing();

    forge_ops_tracker::Configuration& config = forge_ops_tracker::init([&server](Configuration& c) {
        c.dsn = dsn_for(server);
        c.environment = "production";
    });
    ASSERT_TRUE(config.environment == "production");

    forge_ops_tracker::capture_exception(fot_test_exc::BoomError("boom"));
    ASSERT_TRUE(server.wait_for_request_count(1));

    forge_ops_tracker::reset_for_testing();
}

TEST(tracker_set_user_attaches_the_user_to_a_later_capture_exception_call) {
    TestServer server(202);
    forge_ops_tracker::reset_for_testing();

    forge_ops_tracker::init([&server](Configuration& c) {
        c.dsn = dsn_for(server);
        c.environment = "production";
    });

    forge_ops_tracker::set_user({{"id", 42}, {"email", "alice@example.com"}});
    forge_ops_tracker::capture_exception(fot_test_exc::BoomError("boom"));

    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(server.last_request().find("alice@example.com") != std::string::npos);

    forge_ops_tracker::reset_for_testing();
}

TEST(tracker_an_explicit_user_argument_overrides_whatever_set_user_last_set) {
    TestServer server(202);
    forge_ops_tracker::reset_for_testing();

    forge_ops_tracker::init([&server](Configuration& c) {
        c.dsn = dsn_for(server);
        c.environment = "production";
    });

    forge_ops_tracker::set_user({{"id", 42}});
    forge_ops_tracker::capture_exception(fot_test_exc::BoomError("boom"), nlohmann::json::object(), {{"id", 99}});

    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(server.last_request().find("\"id\":99") != std::string::npos);

    forge_ops_tracker::reset_for_testing();
}

TEST(tracker_reset_for_testing_clears_the_current_user) {
    TestServer server(202);
    forge_ops_tracker::set_user({{"id", 42}});
    forge_ops_tracker::reset_for_testing();

    forge_ops_tracker::init([&server](Configuration& c) {
        c.dsn = dsn_for(server);
        c.environment = "production";
    });
    forge_ops_tracker::capture_exception(fot_test_exc::BoomError("boom"));

    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(server.last_request().find("\"user\"") == std::string::npos);

    forge_ops_tracker::reset_for_testing();
}

// TestServer::last_request() is the whole raw HTTP request; the JSON payload is what follows the
// blank line ending its headers.
static std::string request_body(TestServer& server) {
    std::string request = server.last_request();
    auto split = request.find("\r\n\r\n");
    return split == std::string::npos ? request : request.substr(split + 4);
}

static void tracker_init_for(TestServer& server, const std::function<void(Configuration&)>& extra = nullptr) {
    forge_ops_tracker::reset_for_testing();
    forge_ops_tracker::init([&server, &extra](Configuration& c) {
        c.dsn = dsn_for(server);
        c.environment = "production";
        if (extra) {
            extra(c);
        }
    });
}

TEST(tracker_add_breadcrumb_attaches_the_trail_to_a_later_capture_exception_call) {
    TestServer server(202);
    tracker_init_for(server);

    forge_ops_tracker::add_breadcrumb("charging card", "payment", "info", {{"order_id", 42}});
    forge_ops_tracker::capture_exception(fot_test_exc::BoomError("boom"));

    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(server.last_request().find("charging card") != std::string::npos);
    ASSERT_TRUE(server.last_request().find("\"category\":\"payment\"") != std::string::npos);

    forge_ops_tracker::reset_for_testing();
}

TEST(tracker_add_breadcrumb_defaults_to_the_custom_category_and_info_level) {
    TestServer server(202);
    tracker_init_for(server);

    forge_ops_tracker::add_breadcrumb("something happened");
    forge_ops_tracker::capture_exception(fot_test_exc::BoomError("boom"));

    ASSERT_TRUE(server.wait_for_request_count(1));
    nlohmann::json body = nlohmann::json::parse(request_body(server));
    ASSERT_TRUE(body["breadcrumbs"][0]["category"] == "custom");
    ASSERT_TRUE(body["breadcrumbs"][0]["level"] == "info");
    ASSERT_TRUE(body["breadcrumbs"][0]["timestamp"].get<std::string>().size() == 20);

    forge_ops_tracker::reset_for_testing();
}

TEST(tracker_add_breadcrumb_does_nothing_when_track_breadcrumbs_is_off) {
    TestServer server(202);
    tracker_init_for(server, [](Configuration& c) { c.track_breadcrumbs = false; });

    forge_ops_tracker::add_breadcrumb("should not be recorded");
    forge_ops_tracker::capture_exception(fot_test_exc::BoomError("boom"));

    ASSERT_TRUE(server.wait_for_request_count(1));
    // Parsed rather than substring-searched: this test binary's own function names (which show up
    // in the payload's backtrace) contain the word "breadcrumbs".
    ASSERT_TRUE(!nlohmann::json::parse(request_body(server)).contains("breadcrumbs"));

    forge_ops_tracker::reset_for_testing();
}

TEST(tracker_add_breadcrumb_caps_the_trail_at_max_breadcrumbs_dropping_the_oldest_first) {
    TestServer server(202);
    tracker_init_for(server, [](Configuration& c) { c.max_breadcrumbs = 2; });

    forge_ops_tracker::add_breadcrumb("first");
    forge_ops_tracker::add_breadcrumb("second");
    forge_ops_tracker::add_breadcrumb("third");
    forge_ops_tracker::capture_exception(fot_test_exc::BoomError("boom"));

    ASSERT_TRUE(server.wait_for_request_count(1));
    nlohmann::json body = nlohmann::json::parse(request_body(server));
    ASSERT_TRUE(body["breadcrumbs"].size() == 2);
    ASSERT_TRUE(body["breadcrumbs"][0]["message"] == "second");
    ASSERT_TRUE(body["breadcrumbs"][1]["message"] == "third");

    forge_ops_tracker::reset_for_testing();
}

TEST(tracker_clear_breadcrumbs_empties_the_trail) {
    TestServer server(202);
    tracker_init_for(server);

    forge_ops_tracker::add_breadcrumb("first");
    forge_ops_tracker::clear_breadcrumbs();
    forge_ops_tracker::capture_exception(fot_test_exc::BoomError("boom"));

    ASSERT_TRUE(server.wait_for_request_count(1));
    // Parsed rather than substring-searched: this test binary's own function names (which show up
    // in the payload's backtrace) contain the word "breadcrumbs".
    ASSERT_TRUE(!nlohmann::json::parse(request_body(server)).contains("breadcrumbs"));

    forge_ops_tracker::reset_for_testing();
}

TEST(tracker_breadcrumbs_are_isolated_per_thread) {
    TestServer server(202);
    tracker_init_for(server);

    forge_ops_tracker::add_breadcrumb("recorded on the main thread");
    std::thread other([] {
        // A different thread starts with its own empty trail, and what it records never shows up
        // on the main thread's.
        forge_ops_tracker::add_breadcrumb("recorded on another thread");
        forge_ops_tracker::capture_exception(fot_test_exc::BoomError("from other thread"));
    });
    other.join();
    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(server.last_request().find("recorded on another thread") != std::string::npos);
    ASSERT_TRUE(server.last_request().find("recorded on the main thread") == std::string::npos);

    forge_ops_tracker::capture_exception(fot_test_exc::BoomError("from main thread"));
    ASSERT_TRUE(server.wait_for_request_count(2));
    ASSERT_TRUE(server.last_request().find("recorded on the main thread") != std::string::npos);
    ASSERT_TRUE(server.last_request().find("recorded on another thread") == std::string::npos);

    forge_ops_tracker::reset_for_testing();
}

TEST(tracker_reset_for_testing_clears_the_breadcrumb_trail) {
    TestServer server(202);
    forge_ops_tracker::add_breadcrumb("leftover");
    tracker_init_for(server); // calls reset_for_testing first

    forge_ops_tracker::capture_exception(fot_test_exc::BoomError("boom"));

    ASSERT_TRUE(server.wait_for_request_count(1));
    // Parsed rather than substring-searched: this test binary's own function names (which show up
    // in the payload's backtrace) contain the word "breadcrumbs".
    ASSERT_TRUE(!nlohmann::json::parse(request_body(server)).contains("breadcrumbs"));

    forge_ops_tracker::reset_for_testing();
}

TEST(tracker_install_terminate_handler_is_idempotent) {
    /* Must not crash (or double-chain) on a second call: same property sdks/c's own
     * tracker_install_handlers_is_idempotent test checks for its own installer. */
    forge_ops_tracker::install_terminate_handler();
    forge_ops_tracker::install_terminate_handler();
}

/* ---- main ------------------------------------------------------------------------------------- */

static std::string dsn_for_port(int port) {
    return "http://the-api-key@127.0.0.1:" + std::to_string(port) + "/api/v1/events";
}

/* ---- Performance monitoring ----------------------------------------------------------------------- */

static Configuration performance_configuration(int port) {
    Configuration c;
    c.dsn = dsn_for_port(port);
    c.environment = "production";
    c.performance_flush_interval = std::chrono::hours(1); /* tests flush by hand unless they say otherwise */
    c.timeout_seconds = 2;
    return c;
}

TEST(performance_record_buckets_by_transaction_name_with_count_sum_and_max) {
    Configuration config = performance_configuration(1);
    forge_ops_tracker::PerformanceFlusher flusher(config, Client(config));

    flusher.record("GET /users/:id", 10.0);
    flusher.record("GET /users/:id", 30.0);
    flusher.record("POST /orders", 5.0);

    auto users = flusher.tally_for_testing("GET /users/:id");
    ASSERT_TRUE(users.count == 2 && users.duration_sum_ms == 40.0 && users.max_duration_ms == 30.0);
    auto orders = flusher.tally_for_testing("POST /orders");
    ASSERT_TRUE(orders.count == 1 && orders.duration_sum_ms == 5.0);
    flusher.discard();
}

TEST(performance_record_does_nothing_when_track_performance_is_off_or_the_environment_is_not_enabled) {
    Configuration off = performance_configuration(1);
    off.track_performance = false;
    forge_ops_tracker::PerformanceFlusher off_flusher(off, Client(off));
    off_flusher.record("GET /x", 10.0);
    ASSERT_TRUE(off_flusher.tally_for_testing("GET /x").count == 0);
    off_flusher.discard();

    Configuration development = performance_configuration(1);
    development.environment = "development";
    forge_ops_tracker::PerformanceFlusher development_flusher(development, Client(development));
    development_flusher.record("GET /x", 10.0);
    ASSERT_TRUE(development_flusher.tally_for_testing("GET /x").count == 0);
    development_flusher.discard();
}

TEST(performance_flush_delivers_one_batch_to_performance_samples_and_empties_the_buckets) {
    TestServer server(202);
    Configuration config = performance_configuration(server.port());
    config.release = "a1b2c3d";
    forge_ops_tracker::PerformanceFlusher flusher(config, Client(config));
    flusher.record("GET /users/:id", 10.0);
    flusher.record("GET /users/:id", 30.0);

    flusher.flush();

    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(server.last_request().find("POST /api/v1/performance_samples") == 0);
    nlohmann::json body = nlohmann::json::parse(request_body(server));
    const auto& sample = body["samples"][0];
    ASSERT_TRUE(sample["transaction_name"] == "GET /users/:id");
    ASSERT_TRUE(sample["request_count"] == 2);
    ASSERT_TRUE(sample["duration_sum_ms"] == 40.0);
    ASSERT_TRUE(sample["max_duration_ms"] == 30.0);
    ASSERT_TRUE(sample["environment"] == "production");
    ASSERT_TRUE(sample["release"] == "a1b2c3d");
    ASSERT_TRUE(sample["period_started_at"].get<std::string>().back() == 'Z');
    ASSERT_TRUE(flusher.tally_for_testing("GET /users/:id").count == 0);
    flusher.discard();
}

TEST(performance_flush_does_nothing_when_there_is_nothing_to_send) {
    TestServer server(202);
    Configuration config = performance_configuration(server.port());
    forge_ops_tracker::PerformanceFlusher flusher(config, Client(config));

    flusher.flush();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(server.request_count() == 0);
    flusher.discard();
}

TEST(performance_a_failed_delivery_keeps_every_bucket_so_the_next_flush_carries_more) {
    TestServer failing(500);
    TestServer working(202);
    Configuration config = performance_configuration(failing.port());
    forge_ops_tracker::PerformanceFlusher flusher(config, Client(config));
    flusher.record("GET /x", 10.0);

    flusher.flush();
    ASSERT_TRUE(flusher.tally_for_testing("GET /x").count == 1);

    config.dsn = dsn_for_port(working.port());
    flusher.record("GET /x", 20.0);
    flusher.flush();

    ASSERT_TRUE(working.wait_for_request_count(1));
    ASSERT_TRUE(nlohmann::json::parse(request_body(working))["samples"][0]["request_count"] == 2);
    ASSERT_TRUE(flusher.tally_for_testing("GET /x").count == 0);
    flusher.discard();
}

TEST(performance_a_record_that_lands_during_delivery_is_never_lost) {
    /* Deterministic reproduction of the race flush()'s own comment describes: the hook runs
     * strictly between the snapshot and delivery succeeding, exactly where a record from another
     * thread could land. */
    TestServer server(202);
    Configuration config = performance_configuration(server.port());
    forge_ops_tracker::PerformanceFlusher flusher(config, Client(config));
    flusher.record("GET /x", 10.0);
    flusher.set_before_delivery_hook_for_testing([&flusher] {
        flusher.record("GET /x", 25.0);  /* same transaction, mid-delivery */
        flusher.record("GET /new", 7.0); /* a brand-new one, mid-delivery */
    });

    flusher.flush();

    auto x = flusher.tally_for_testing("GET /x");
    ASSERT_TRUE(x.count == 1 && x.duration_sum_ms == 25.0 && x.max_duration_ms == 25.0);
    auto added = flusher.tally_for_testing("GET /new");
    ASSERT_TRUE(added.count == 1 && added.duration_sum_ms == 7.0);
    flusher.discard();
}

TEST(histogram_bucket_for_returns_the_smallest_boundary_a_duration_fits_under_as_a_string) {
    ASSERT_TRUE(forge_ops_tracker::histogram_bucket_for(10) == "50");
    ASSERT_TRUE(forge_ops_tracker::histogram_bucket_for(50) == "50");
    ASSERT_TRUE(forge_ops_tracker::histogram_bucket_for(50.5) == "100");
    ASSERT_TRUE(forge_ops_tracker::histogram_bucket_for(4999) == "5000");
}

TEST(histogram_bucket_for_returns_inf_for_anything_larger_than_the_largest_boundary) {
    ASSERT_TRUE(forge_ops_tracker::histogram_bucket_for(10001) == "inf");
    ASSERT_TRUE(forge_ops_tracker::histogram_bucket_for(1000000) == "inf");
}

TEST(histogram_bucket_for_puts_a_duration_exactly_on_a_boundary_into_that_boundarys_own_bucket) {
    for (unsigned long boundary : forge_ops_tracker::kHistogramBoundariesMs) {
        ASSERT_TRUE(forge_ops_tracker::histogram_bucket_for(static_cast<double>(boundary)) == std::to_string(boundary));
    }
}

TEST(histogram_boundaries_match_the_servers_histogram_percentile) {
    /* app/services/histogram_percentile.rb and every other SDK must agree on this exact list. */
    const std::array<unsigned long, 8> expected = {50, 100, 250, 500, 1000, 2500, 5000, 10000};
    ASSERT_TRUE(forge_ops_tracker::kHistogramBoundariesMs == expected);
}

TEST(performance_flush_delivers_a_latency_histogram_alongside_count_sum_and_max) {
    TestServer server(202);
    Configuration config = performance_configuration(server.port());
    forge_ops_tracker::PerformanceFlusher flusher(config, Client(config));
    for (double duration : {10.0, 40.0, 120.0, 700.0, 12000.0}) {
        flusher.record("GET /posts", duration);
    }

    flusher.flush();

    ASSERT_TRUE(server.wait_for_request_count(1));
    const auto sample = nlohmann::json::parse(request_body(server))["samples"][0];
    ASSERT_TRUE(sample["histogram"] == (nlohmann::json{{"50", 2}, {"250", 1}, {"1000", 1}, {"inf", 1}}));
    ASSERT_TRUE(sample["request_count"] == 5);
    flusher.discard();
}

TEST(performance_a_failed_delivery_keeps_histogram_counts_for_the_next_flush) {
    TestServer failing(500);
    TestServer working(202);
    Configuration config = performance_configuration(failing.port());
    forge_ops_tracker::PerformanceFlusher flusher(config, Client(config));
    flusher.record("GET /posts", 10.0);
    flusher.flush(); /* the first delivery fails: the histogram must survive it */

    config.dsn = dsn_for_port(working.port());
    flusher.record("GET /posts", 300.0);
    flusher.flush();

    ASSERT_TRUE(working.wait_for_request_count(1));
    const auto sample = nlohmann::json::parse(request_body(working))["samples"][0];
    ASSERT_TRUE(sample["histogram"] == (nlohmann::json{{"50", 1}, {"500", 1}}));
    flusher.discard();
}

TEST(performance_a_histogram_count_recorded_during_delivery_is_sent_on_the_next_flush) {
    TestServer server(202);
    Configuration config = performance_configuration(server.port());
    forge_ops_tracker::PerformanceFlusher flusher(config, Client(config));
    flusher.record("GET /posts", 10.0);
    flusher.set_before_delivery_hook_for_testing([&flusher] {
        flusher.set_before_delivery_hook_for_testing({});
        flusher.record("GET /posts", 300.0); /* same transaction, mid-delivery */
        flusher.record("GET /new", 5.0);     /* a brand-new one, mid-delivery */
    });

    flusher.flush();
    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(nlohmann::json::parse(request_body(server))["samples"][0]["histogram"] == (nlohmann::json{{"50", 1}}));

    flusher.flush();
    ASSERT_TRUE(server.wait_for_request_count(2));
    const auto second = nlohmann::json::parse(request_body(server))["samples"];
    ASSERT_TRUE(second.size() == 2);
    for (const auto& sample : second) {
        if (sample["transaction_name"] == "GET /posts") {
            ASSERT_TRUE(sample["histogram"] == (nlohmann::json{{"500", 1}}));
        } else {
            ASSERT_TRUE(sample["transaction_name"] == "GET /new");
            ASSERT_TRUE(sample["histogram"] == (nlohmann::json{{"50", 1}}));
        }
    }
    flusher.discard();
}

TEST(performance_the_background_thread_flushes_on_its_own_interval) {
    TestServer server(202);
    Configuration config = performance_configuration(server.port());
    config.performance_flush_interval = std::chrono::milliseconds(50);
    forge_ops_tracker::PerformanceFlusher flusher(config, Client(config));

    flusher.record("GET /x", 10.0);

    ASSERT_TRUE(server.wait_for_request_count(1, 3000));
    ASSERT_TRUE(request_body(server).find("\"transaction_name\":\"GET /x\"") != std::string::npos);
    flusher.discard();
}

TEST(performance_destroying_a_flusher_delivers_whatever_is_left_but_discard_does_not) {
    TestServer server(202);
    Configuration config = performance_configuration(server.port());
    {
        forge_ops_tracker::PerformanceFlusher discarded(config, Client(config));
        discarded.record("GET /discarded", 10.0);
        discarded.discard();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(server.request_count() == 0);

    {
        forge_ops_tracker::PerformanceFlusher leaving(config, Client(config));
        leaving.record("GET /last-window", 10.0);
    }
    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(request_body(server).find("GET /last-window") != std::string::npos);
}

TEST(performance_samples_uri_swaps_the_trailing_events_segment) {
    Configuration config;
    config.dsn = "https://key@tracker.example.com/api/v1/events";
    ASSERT_TRUE(*config.performance_samples_uri() == "https://tracker.example.com/api/v1/performance_samples");

    config.dsn = std::nullopt;
    ASSERT_TRUE(!config.performance_samples_uri());
}

TEST(tracker_record_performance_and_flush_performance_deliver_through_the_full_stack) {
    TestServer server(202);
    tracker_init_for(server, [](Configuration& c) { c.performance_flush_interval = std::chrono::hours(1); });

    forge_ops_tracker::record_performance("GET /users/:id", 10.0);
    forge_ops_tracker::record_performance("GET /users/:id", 30.0);
    forge_ops_tracker::flush_performance();

    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(nlohmann::json::parse(request_body(server))["samples"][0]["request_count"] == 2);
    forge_ops_tracker::reset_for_testing();
}

TEST(tracker_time_transaction_returns_the_callables_value_and_records_how_long_it_took) {
    TestServer server(202);
    tracker_init_for(server, [](Configuration& c) { c.performance_flush_interval = std::chrono::hours(1); });

    int value = forge_ops_tracker::time_transaction("timed", [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        return 42;
    });
    forge_ops_tracker::time_transaction("timed void", [] {}); /* a callable returning void compiles and works too */
    forge_ops_tracker::flush_performance();

    ASSERT_TRUE(value == 42);
    ASSERT_TRUE(server.wait_for_request_count(1));
    nlohmann::json body = nlohmann::json::parse(request_body(server));
    bool saw_timed = false;
    for (const auto& sample : body["samples"]) {
        if (sample["transaction_name"] == "timed") {
            saw_timed = true;
            ASSERT_TRUE(sample["duration_sum_ms"].get<double>() >= 25.0);
        }
    }
    ASSERT_TRUE(saw_timed);
    forge_ops_tracker::reset_for_testing();
}

TEST(tracker_scoped_transaction_records_even_when_the_scope_throws) {
    TestServer server(202);
    tracker_init_for(server, [](Configuration& c) { c.performance_flush_interval = std::chrono::hours(1); });

    try {
        forge_ops_tracker::ScopedTransaction timing("timed throws");
        throw std::runtime_error("inside");
    } catch (const std::runtime_error&) {
    }
    forge_ops_tracker::flush_performance();

    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(request_body(server).find("\"transaction_name\":\"timed throws\"") != std::string::npos);
    forge_ops_tracker::reset_for_testing();
}

TEST(tracker_track_performance_off_records_and_delivers_nothing) {
    TestServer server(202);
    tracker_init_for(server, [](Configuration& c) { c.track_performance = false; });

    forge_ops_tracker::record_performance("never recorded", 10.0);
    forge_ops_tracker::flush_performance();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(server.request_count() == 0);
    forge_ops_tracker::reset_for_testing();
}

/* ---- Distributed tracing -------------------------------------------------------------------------- */

static Configuration tracing_configuration() {
    Configuration c;
    c.dsn = dsn_for_port(1);
    c.environment = "production";
    c.release = "a1b2c3d";
    c.trace_capture_threshold = std::chrono::milliseconds(10);
    return c;
}

static nlohmann::json span_named(const nlohmann::json& trace, const std::string& name) {
    for (const auto& span : trace["spans"]) {
        if (span["name"] == name) {
            return span;
        }
    }
    return nlohmann::json();
}

TEST(tracing_span_buffer_nests_spans_under_the_open_one_and_the_root_with_the_wire_shape) {
    Configuration config = tracing_configuration();
    forge_ops_tracker::SpanBuffer buffer(config);
    auto at = std::chrono::system_clock::time_point(std::chrono::milliseconds(1700000000123LL));

    std::string outer = buffer.open();
    buffer.record_leaf("SELECT users", "database", at, 3.0, nlohmann::json::object());
    buffer.close(outer, "charge", "service", at, 20.0, {{"order", 42}});
    buffer.record_leaf("sibling", "database", at, 1.0, nlohmann::json::object());

    auto trace = buffer.finish("GET /checkout", at, 1500.0);
    ASSERT_TRUE(trace.has_value());
    ASSERT_TRUE((*trace)["trace_id"].get<std::string>().size() == 32);
    auto root = span_named(*trace, "GET /checkout");
    auto charge = span_named(*trace, "charge");
    ASSERT_TRUE(root["parent_span_id"].is_null());
    ASSERT_TRUE(root["kind"] == "controller");
    ASSERT_TRUE(root["span_id"].get<std::string>().size() == 16);
    ASSERT_TRUE(charge["parent_span_id"] == root["span_id"]);
    ASSERT_TRUE(span_named(*trace, "SELECT users")["parent_span_id"] == charge["span_id"]);
    ASSERT_TRUE(span_named(*trace, "sibling")["parent_span_id"] == root["span_id"]);
    ASSERT_TRUE(charge["started_at"] == "2023-11-14T22:13:20.123Z");
    ASSERT_TRUE(charge["environment"] == "production" && charge["release"] == "a1b2c3d");
    ASSERT_TRUE(charge["data"]["order"] == 42);
    ASSERT_TRUE(root["data"].is_object() && root["data"].empty());
}

TEST(tracing_span_buffer_sends_an_unknown_kind_as_other_since_the_server_would_reject_the_whole_trace) {
    Configuration config = tracing_configuration();
    forge_ops_tracker::SpanBuffer buffer(config);
    auto at = std::chrono::system_clock::now();
    buffer.record_leaf("q", "db", at, 1.0, nlohmann::json::object());
    buffer.record_leaf("r", "database", at, 1.0, nlohmann::json::object());

    auto trace = buffer.finish("root", at, 500.0);
    ASSERT_TRUE(span_named(*trace, "q")["kind"] == "other");
    ASSERT_TRUE(span_named(*trace, "r")["kind"] == "database");
}

TEST(tracing_span_buffer_drops_a_trace_under_the_threshold_and_caps_a_big_one_at_500_spans) {
    Configuration config = tracing_configuration();
    config.trace_capture_threshold = std::chrono::milliseconds(1000);
    forge_ops_tracker::SpanBuffer fast(config);
    ASSERT_TRUE(!fast.finish("GET /fast", std::chrono::system_clock::now(), 999.0).has_value());

    forge_ops_tracker::SpanBuffer big(config);
    for (int i = 0; i < 700; i++) {
        big.record_leaf("q", "database", std::chrono::system_clock::now(), 1.0, nlohmann::json::object());
    }
    ASSERT_TRUE(big.finish("GET /x", std::chrono::system_clock::now(), 2000.0)->at("spans").size() == 500);
}

TEST(tracing_a_slow_scoped_trace_is_delivered_to_spans_with_nested_spans) {
    TestServer server(202);
    tracker_init_for(server, [](Configuration& c) { c.trace_capture_threshold = std::chrono::milliseconds(10); });

    {
        forge_ops_tracker::ScopedTrace trace("GET /checkout");
        forge_ops_tracker::ScopedSpan charge("charge card", "service", {{"order_id", 42}});
        forge_ops_tracker::record_span("SELECT orders", "database", std::chrono::system_clock::now(), 3.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(server.last_request().find("POST /api/v1/spans") == 0);
    nlohmann::json body = nlohmann::json::parse(request_body(server));
    auto root = span_named(body, "GET /checkout");
    auto charge = span_named(body, "charge card");
    ASSERT_TRUE(root["parent_span_id"].is_null());
    ASSERT_TRUE(charge["parent_span_id"] == root["span_id"]);
    ASSERT_TRUE(span_named(body, "SELECT orders")["parent_span_id"] == charge["span_id"]);
    ASSERT_TRUE(charge["data"]["order_id"] == 42);
    forge_ops_tracker::reset_for_testing();
}

TEST(tracing_a_scope_that_throws_is_still_recorded_and_sent) {
    TestServer server(202);
    tracker_init_for(server, [](Configuration& c) { c.trace_capture_threshold = std::chrono::milliseconds(10); });

    try {
        forge_ops_tracker::ScopedTrace trace("GET /boom");
        forge_ops_tracker::ScopedSpan bad("bad", "service");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        throw std::runtime_error("boom");
    } catch (const std::runtime_error&) {
    }

    ASSERT_TRUE(server.wait_for_request_count(1));
    nlohmann::json body = nlohmann::json::parse(request_body(server));
    ASSERT_TRUE(!span_named(body, "bad").is_null());
    forge_ops_tracker::reset_for_testing();
}

TEST(tracing_a_fast_trace_sends_nothing) {
    TestServer server(202);
    tracker_init_for(server, [](Configuration& c) { c.trace_capture_threshold = std::chrono::milliseconds(60000); });

    {
        forge_ops_tracker::ScopedTrace trace("GET /fast");
        forge_ops_tracker::ScopedSpan span("x");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ASSERT_TRUE(server.request_count() == 0);
    forge_ops_tracker::reset_for_testing();
}

TEST(tracing_track_tracing_off_or_reporting_disabled_records_and_sends_nothing) {
    TestServer server(202);
    tracker_init_for(server, [](Configuration& c) {
        c.trace_capture_threshold = std::chrono::milliseconds(10);
        c.track_tracing = false;
    });
    {
        forge_ops_tracker::ScopedTrace trace("GET /x");
        forge_ops_tracker::ScopedSpan span("y");
        forge_ops_tracker::record_span("z", "database", std::chrono::system_clock::now(), 1.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    tracker_init_for(server, [](Configuration& c) {
        c.trace_capture_threshold = std::chrono::milliseconds(10);
        c.environment = "development";
    });
    {
        forge_ops_tracker::ScopedTrace trace("GET /x");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ASSERT_TRUE(server.request_count() == 0);
    forge_ops_tracker::reset_for_testing();
}

TEST(tracing_a_scoped_trace_nested_inside_a_trace_records_a_span_instead_of_starting_a_second_trace) {
    TestServer server(202);
    tracker_init_for(server, [](Configuration& c) { c.trace_capture_threshold = std::chrono::milliseconds(10); });

    {
        forge_ops_tracker::ScopedTrace outer("outer");
        {
            forge_ops_tracker::ScopedTrace inner("inner");
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }

    ASSERT_TRUE(server.wait_for_request_count(1));
    nlohmann::json body = nlohmann::json::parse(request_body(server));
    ASSERT_TRUE(body["spans"].size() == 2);
    ASSERT_TRUE(span_named(body, "inner")["parent_span_id"] == span_named(body, "outer")["span_id"]);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(server.request_count() == 1);
    forge_ops_tracker::reset_for_testing();
}

TEST(tracing_trace_and_span_return_the_callables_value_and_a_span_outside_a_trace_just_runs) {
    forge_ops_tracker::reset_for_testing();
    ASSERT_TRUE(forge_ops_tracker::span("free", "service", [] { return 7; }) == 7);
    forge_ops_tracker::span("free void", "service", [] {}); /* a callable returning void compiles and works too */
    forge_ops_tracker::record_span("free", "database", std::chrono::system_clock::now(), 1.0); /* a no-op outside a trace */
    ASSERT_TRUE(forge_ops_tracker::trace("t", [] { return 42; }) == 42);
    forge_ops_tracker::reset_for_testing();
}

TEST(tracing_the_open_trace_is_per_thread) {
    TestServer server(202);
    tracker_init_for(server, [](Configuration& c) { c.trace_capture_threshold = std::chrono::milliseconds(10); });

    {
        forge_ops_tracker::ScopedTrace trace("main thread");
        std::thread other([] {
            /* No trace is open on this thread: this span records nothing, and no trace leaks in. */
            forge_ops_tracker::ScopedSpan span("other thread's span");
        });
        other.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    ASSERT_TRUE(server.wait_for_request_count(1));
    nlohmann::json body = nlohmann::json::parse(request_body(server));
    ASSERT_TRUE(body["spans"].size() == 1);
    forge_ops_tracker::reset_for_testing();
}

TEST(tracing_spans_uri_swaps_the_trailing_events_segment) {
    Configuration config = tracing_configuration();
    ASSERT_TRUE(*config.spans_uri() == "http://127.0.0.1:1/api/v1/spans");
}

/* ---- Trace context (W3C traceparent) ------------------------------------------------------------ */

namespace tp = forge_ops_tracker::trace_parent;
static const std::string kTraceId = "4bf92f3577b34da6a3ce929d0e0e4736";
static const std::string kSpanId = "00f067aa0ba902b7";

TEST(traceparent_parses_a_valid_version_00_header) {
    auto context = tp::parse("00-" + kTraceId + "-" + kSpanId + "-01");
    ASSERT_TRUE(context.has_value());
    ASSERT_TRUE(context->trace_id == kTraceId && context->parent_span_id == kSpanId);
    ASSERT_TRUE(tp::parse("  00-" + kTraceId + "-" + kSpanId + "-00 ").has_value());
}

TEST(traceparent_rejects_anything_malformed) {
    std::string upper_trace = kTraceId;
    for (char& c : upper_trace) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const std::vector<std::string> bad = {
        "",
        "garbage",
        "00-" + upper_trace + "-" + kSpanId + "-01",
        "00-" + kTraceId + "-00F067AA0BA902B7-01",
        "ff-" + kTraceId + "-" + kSpanId + "-01",
        "00-" + std::string(32, '0') + "-" + kSpanId + "-01",
        "00-" + kTraceId + "-" + std::string(16, '0') + "-01",
        "00-" + kTraceId.substr(0, 31) + "-" + kSpanId + "-01",
        "00-" + kTraceId + "-" + kSpanId.substr(0, 15) + "-01",
        "00_" + kTraceId + "-" + kSpanId + "-01",
        "00-" + kTraceId + "-" + kSpanId + "-1",
        "00-" + kTraceId + "-" + kSpanId + "-01-extra",
        "0g-" + kTraceId + "-" + kSpanId + "-01",
        "01-" + kTraceId + "-" + kSpanId + "-01x",
    };
    for (const auto& value : bad) {
        if (tp::parse(value)) {
            std::printf("  accepted \"%s\"\n", value.c_str());
            ASSERT_TRUE(false);
        }
    }
}

TEST(traceparent_accepts_a_future_version_with_extra_fields) {
    auto context = tp::parse("01-" + kTraceId + "-" + kSpanId + "-01-what-comes-next");
    ASSERT_TRUE(context && context->trace_id == kTraceId);
    ASSERT_TRUE(tp::parse("01-" + kTraceId + "-" + kSpanId + "-01").has_value());
}

TEST(traceparent_builds_a_sampled_version_00_header_and_ids_are_lowercase_hex_never_all_zeros) {
    ASSERT_TRUE(tp::build(kTraceId, kSpanId) == "00-" + kTraceId + "-" + kSpanId + "-01");
    std::string trace_id = tp::generate_trace_id();
    std::string span_id = tp::generate_span_id();
    ASSERT_TRUE(trace_id.size() == 32 && span_id.size() == 16);
    ASSERT_TRUE(trace_id.find_first_not_of("0123456789abcdef") == std::string::npos);
    ASSERT_TRUE(span_id.find_first_not_of("0123456789abcdef") == std::string::npos);
    ASSERT_TRUE(trace_id.find_first_not_of('0') != std::string::npos);
}

TEST(traceparent_url_host_extracts_just_the_lowercased_host) {
    ASSERT_TRUE(tp::url_host("https://API.Example.com/orders/42?x=1") == std::optional<std::string>("api.example.com"));
    ASSERT_TRUE(tp::url_host("http://user:pw@example.com:8080/x") == std::optional<std::string>("example.com"));
    ASSERT_TRUE(tp::url_host("http://example.com?q=a@b") == std::optional<std::string>("example.com"));
    ASSERT_TRUE(tp::url_host("http://[::1]:3000/") == std::optional<std::string>("[::1]"));
    ASSERT_TRUE(!tp::url_host("example.com/no-scheme"));
    ASSERT_TRUE(!tp::url_host("http:///path-only"));
    ASSERT_TRUE(!tp::url_host(""));
}

TEST(trace_propagation_goes_to_every_host_by_default_and_nowhere_when_off) {
    Configuration config;
    ASSERT_TRUE(config.propagate_traces);
    ASSERT_TRUE(!config.trace_propagation_targets.has_value());
    ASSERT_TRUE(config.should_propagate_trace(std::string("anything.example")));
    ASSERT_TRUE(config.should_propagate_trace(std::nullopt));
    config.propagate_traces = false;
    ASSERT_TRUE(!config.should_propagate_trace(std::string("anything.example")));
}

TEST(trace_propagation_targets_match_hosts_on_a_dot_boundary_and_search_regexes_in_the_host) {
    Configuration config;
    config.trace_propagation_targets = std::vector<forge_ops_tracker::TracePropagationTarget>{
        "Example.com", std::string(".internal.corp"), "", std::regex(R"(^svc-\d+\.local$)")};
    ASSERT_TRUE(config.should_propagate_trace(std::string("example.com")));
    ASSERT_TRUE(config.should_propagate_trace(std::string("API.example.COM")));
    ASSERT_TRUE(config.should_propagate_trace(std::string("internal.corp")));
    ASSERT_TRUE(config.should_propagate_trace(std::string("db.internal.corp")));
    ASSERT_TRUE(config.should_propagate_trace(std::string("SVC-12.local")));
    ASSERT_TRUE(!config.should_propagate_trace(std::string("svc-x.local")));
    ASSERT_TRUE(!config.should_propagate_trace(std::string("badexample.com")));
    ASSERT_TRUE(!config.should_propagate_trace(std::string("example.com.evil.net")));
    ASSERT_TRUE(!config.should_propagate_trace(std::string("other.net")));
    ASSERT_TRUE(!config.should_propagate_trace(std::string("examplf.com"))); /* same length as a target, not equal */
    ASSERT_TRUE(!config.should_propagate_trace(std::nullopt));

    config.trace_propagation_targets = std::vector<forge_ops_tracker::TracePropagationTarget>{};
    ASSERT_TRUE(!config.should_propagate_trace(std::string("example.com")));
}

TEST(trace_context_span_buffer_continues_an_incoming_trace_and_never_sends_with_track_tracing_off) {
    Configuration config = tracing_configuration();
    forge_ops_tracker::SpanBuffer continued(config, tp::parse("00-" + kTraceId + "-" + kSpanId + "-01"));
    ASSERT_TRUE(continued.trace_id() == kTraceId);
    auto trace = continued.finish("POST /orders", std::chrono::system_clock::now(), 500.0);
    ASSERT_TRUE(trace && (*trace)["trace_id"] == kTraceId);
    ASSERT_TRUE(span_named(*trace, "POST /orders")["parent_span_id"] == kSpanId);

    config.track_tracing = false;
    forge_ops_tracker::SpanBuffer off(config);
    ASSERT_TRUE(off.trace_id().size() == 32);
    ASSERT_TRUE(!off.finish("x", std::chrono::system_clock::now(), 500.0).has_value());
}

TEST(trace_context_a_continued_trace_is_sent_under_the_callers_span_and_the_http_span_names_itself_in_the_header) {
    TestServer server(202);
    tracker_init_for(server, [](Configuration& c) { c.trace_capture_threshold = std::chrono::milliseconds(10); });

    std::optional<std::string> header;
    std::string http_id;
    {
        forge_ops_tracker::ScopedTrace trace("POST /orders", "00-" + kTraceId + "-" + kSpanId + "-01");
        ASSERT_TRUE(forge_ops_tracker::current_trace_id() == std::optional<std::string>(kTraceId));
        forge_ops_tracker::ScopedHttpSpan http("post", "https://Payments.example.com/charges/42?token=secret");
        header = http.traceparent();
        http_id = http.span_id();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    ASSERT_TRUE(!forge_ops_tracker::current_trace_id());
    ASSERT_TRUE(header == std::optional<std::string>("00-" + kTraceId + "-" + http_id + "-01"));

    ASSERT_TRUE(server.wait_for_request_count(1));
    nlohmann::json body = nlohmann::json::parse(request_body(server));
    ASSERT_TRUE(body["trace_id"] == kTraceId);
    auto root = span_named(body, "POST /orders");
    auto http = span_named(body, "POST payments.example.com");
    ASSERT_TRUE(root["parent_span_id"] == kSpanId);
    ASSERT_TRUE(http["span_id"] == http_id);
    ASSERT_TRUE(http["parent_span_id"] == root["span_id"]);
    ASSERT_TRUE(http["kind"] == "http");
    ASSERT_TRUE(request_body(server).find("secret") == std::string::npos);
    forge_ops_tracker::reset_for_testing();
}

TEST(trace_context_a_missing_or_malformed_traceparent_starts_a_fresh_trace) {
    TestServer server(202);
    tracker_init_for(server);
    forge_ops_tracker::trace("GET /x", std::string("00-nope"), [] {
        auto id = forge_ops_tracker::current_trace_id();
        if (!id || id->size() != 32 || *id == kTraceId) g_current_test_failed = true;
        /* ignored inside an already-open trace */
        forge_ops_tracker::ScopedTrace inner("inner", "00-" + kTraceId + "-" + kSpanId + "-01");
        if (forge_ops_tracker::current_trace_id() == std::optional<std::string>(kTraceId)) g_current_test_failed = true;
    });
    ASSERT_TRUE(forge_ops_tracker::trace("GET /y", std::nullopt, [] { return forge_ops_tracker::current_trace_id()->size(); }) == 32);
    forge_ops_tracker::reset_for_testing();
}

TEST(trace_context_no_header_outside_a_trace_off_target_or_with_propagation_off) {
    TestServer server(202);
    tracker_init_for(server, [](Configuration& c) {
        c.trace_propagation_targets = std::vector<forge_ops_tracker::TracePropagationTarget>{"example.com"};
    });

    int value = forge_ops_tracker::http_span("GET", "https://api.example.com/", [](const std::optional<std::string>& traceparent) {
        return traceparent ? 1 : 7;
    });
    ASSERT_TRUE(value == 7);
    {
        forge_ops_tracker::ScopedHttpSpan outside("GET", "https://api.example.com/");
        ASSERT_TRUE(!outside.traceparent() && outside.span_id().empty());
    }

    {
        forge_ops_tracker::ScopedTrace trace("GET /x");
        forge_ops_tracker::ScopedHttpSpan off_target("GET", "https://badexample.com/");
        ASSERT_TRUE(!off_target.traceparent() && !off_target.span_id().empty());
        forge_ops_tracker::ScopedHttpSpan on_target("GET", "https://api.example.com/");
        ASSERT_TRUE(on_target.traceparent().has_value());
    }

    forge_ops_tracker::init([](Configuration& c) {
        c.trace_propagation_targets.reset();
        c.propagate_traces = false;
    });
    {
        forge_ops_tracker::ScopedTrace trace("GET /x");
        forge_ops_tracker::ScopedHttpSpan off("GET", "https://api.example.com/");
        ASSERT_TRUE(!off.traceparent());
    }
    forge_ops_tracker::reset_for_testing();
}

TEST(trace_context_an_error_captured_inside_a_trace_carries_its_id_even_with_track_tracing_off) {
    TestServer server(202);
    tracker_init_for(server, [](Configuration& c) { c.track_tracing = false; });
    {
        forge_ops_tracker::ScopedTrace trace("POST /orders", "00-" + kTraceId + "-" + kSpanId + "-01");
        forge_ops_tracker::capture_exception(fot_test_exc::BoomError("boom"));
    }
    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(nlohmann::json::parse(request_body(server))["trace_id"] == kTraceId);

    forge_ops_tracker::capture_exception(fot_test_exc::BoomError("outside"));
    ASSERT_TRUE(server.wait_for_request_count(2));
    ASSERT_TRUE(!nlohmann::json::parse(request_body(server)).contains("trace_id"));
    forge_ops_tracker::reset_for_testing();
}

TEST(trace_context_event_builder_attaches_trace_id_unscrubbed_and_omits_it_otherwise) {
    Configuration config;
    EventBuilder builder(config);
    nlohmann::json with, without;
    try {
        throw fot_test_exc::BoomError("boom");
    } catch (const std::exception& e) {
        with = builder.build(e, nlohmann::json::object(), nlohmann::json::object(), nlohmann::json::array(), "", kTraceId);
        without = builder.build(e);
    }
    ASSERT_TRUE(with["trace_id"] == kTraceId);
    ASSERT_TRUE(!without.contains("trace_id"));
}

/* ---- Custom metrics and infrastructure monitoring ------------------------------------------------- */

static Configuration metrics_configuration(int port) {
    Configuration c;
    c.dsn = dsn_for_port(port);
    c.environment = "production";
    c.release = "a1b2c3d";
    c.server_name = "web-1";
    c.timeout_seconds = 2;
    c.metric_flush_interval = std::chrono::hours(1); /* tests flush by hand unless they say otherwise */
    c.infrastructure_metric_flush_interval = std::chrono::hours(1);
    return c;
}

static std::unique_ptr<forge_ops_tracker::MetricBuffer> custom_buffer(const Configuration& config) {
    Client client(config);
    return std::make_unique<forge_ops_tracker::MetricBuffer>(
        config, [client](const nlohmann::json& entries) { return client.deliver_metrics(entries); },
        [&config] { return config.metric_flush_interval; });
}

TEST(metrics_flush_delivers_every_entry_as_one_batch_to_custom_metrics_with_the_wire_shape) {
    TestServer server(202);
    Configuration config = metrics_configuration(server.port());
    auto buffer = custom_buffer(config);

    ASSERT_TRUE(buffer->record({{"metric_name", "signup"}, {"value", 1.0}, {"environment", "production"}, {"release", "a1b2c3d"}}));
    ASSERT_TRUE(buffer->record({{"metric_name", "refund"}, {"value", -12.5}}));
    buffer->flush();

    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(server.last_request().find("POST /api/v1/custom_metrics") == 0);
    nlohmann::json body = nlohmann::json::parse(request_body(server));
    ASSERT_TRUE(body["metrics"].size() == 2);
    ASSERT_TRUE(body["metrics"][0]["metric_name"] == "signup");
    ASSERT_TRUE(body["metrics"][0]["release"] == "a1b2c3d");
    ASSERT_TRUE(body["metrics"][1]["value"] == -12.5);
    ASSERT_TRUE(std::regex_match(body["metrics"][0]["recorded_at"].get<std::string>(), std::regex("\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\dZ")));
    ASSERT_TRUE(buffer->size_for_testing() == 0);
}

TEST(metrics_a_nan_or_infinite_or_non_numeric_value_is_dropped) {
    Configuration config = metrics_configuration(1);
    auto buffer = custom_buffer(config);
    ASSERT_TRUE(!buffer->record({{"metric_name", "nan"}, {"value", std::nan("")}}));
    ASSERT_TRUE(!buffer->record({{"metric_name", "inf"}, {"value", std::numeric_limits<double>::infinity()}}));
    ASSERT_TRUE(!buffer->record({{"metric_name", "str"}, {"value", "12"}}));
    ASSERT_TRUE(!buffer->record({{"metric_name", "missing"}}));
    ASSERT_TRUE(buffer->record({{"metric_name", "ok"}, {"value", 3}}));
    buffer->discard();
}

TEST(metrics_a_failed_delivery_keeps_every_entry_so_the_next_flush_carries_more) {
    TestServer failing(500);
    TestServer working(202);
    Configuration config = metrics_configuration(failing.port());
    auto buffer = custom_buffer(config);
    buffer->record({{"metric_name", "a"}, {"value", 1}});

    buffer->flush();
    ASSERT_TRUE(buffer->size_for_testing() == 1);

    config.dsn = dsn_for_port(working.port());
    buffer->record({{"metric_name", "b"}, {"value", 2}});
    buffer->flush();

    ASSERT_TRUE(working.wait_for_request_count(1));
    ASSERT_TRUE(nlohmann::json::parse(request_body(working))["metrics"].size() == 2);
    ASSERT_TRUE(buffer->size_for_testing() == 0);
}

TEST(metrics_an_entry_recorded_while_delivery_is_in_flight_is_never_lost) {
    TestServer server(202);
    Configuration config = metrics_configuration(server.port());
    auto buffer = custom_buffer(config);
    buffer->record({{"metric_name", "first"}, {"value", 1}});
    auto* raw = buffer.get();
    buffer->set_before_delivery_hook_for_testing([raw] { raw->record({{"metric_name", "during"}, {"value", 2}}); });

    buffer->flush();

    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(nlohmann::json::parse(request_body(server))["metrics"].size() == 1);
    ASSERT_TRUE(buffer->size_for_testing() == 1);
}

TEST(metrics_a_buffer_is_capped_and_drops_further_entries_until_a_flush_succeeds) {
    Configuration config = metrics_configuration(1);
    auto buffer = custom_buffer(config);
    int accepted = 0;
    for (std::size_t i = 0; i < forge_ops_tracker::MetricBuffer::max_entries + 50; i++) {
        if (buffer->record({{"metric_name", "m"}, {"value", 1}})) {
            accepted++;
        }
    }
    ASSERT_TRUE(static_cast<std::size_t>(accepted) == forge_ops_tracker::MetricBuffer::max_entries);
    buffer->discard();
}

TEST(metrics_the_background_thread_flushes_on_its_own_interval) {
    TestServer server(202);
    Configuration config = metrics_configuration(server.port());
    config.metric_flush_interval = std::chrono::milliseconds(50);
    auto buffer = custom_buffer(config);

    buffer->record({{"metric_name", "tick"}, {"value", 1}});

    ASSERT_TRUE(server.wait_for_request_count(1, 3000));
    ASSERT_TRUE(request_body(server).find("tick") != std::string::npos);
}

TEST(metrics_destroying_a_buffer_delivers_whatever_is_left_but_discard_does_not) {
    TestServer server(202);
    Configuration config = metrics_configuration(server.port());
    {
        auto discarded = custom_buffer(config);
        discarded->record({{"metric_name", "discarded"}, {"value", 1}});
        discarded->discard();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(server.request_count() == 0);

    {
        auto kept = custom_buffer(config);
        kept->record({{"metric_name", "last words"}, {"value", 1}});
    }
    ASSERT_TRUE(server.wait_for_request_count(1));
    ASSERT_TRUE(request_body(server).find("last words") != std::string::npos);
}

TEST(metrics_capture_metric_and_capture_infrastructure_metric_deliver_to_their_own_endpoints_through_the_full_stack) {
    TestServer server(202);
    tracker_init_for(server, [](Configuration& c) {
        c.release = "a1b2c3d";
        c.server_name = "web-1";
        c.metric_flush_interval = std::chrono::hours(1);
        c.infrastructure_metric_flush_interval = std::chrono::hours(1);
    });

    forge_ops_tracker::capture_metric("signup");
    forge_ops_tracker::capture_metric("payment", 49.0);
    forge_ops_tracker::capture_infrastructure_metric("cpu", 0.42, "db-1");
    forge_ops_tracker::capture_infrastructure_metric("memory", 0.7);
    forge_ops_tracker::flush_metrics();

    ASSERT_TRUE(server.wait_for_request_count(2));
    std::vector<std::string> requests;
    for (int i = 0; i < 2; i++) {
        requests.push_back(server.request_at(i));
    }
    std::string custom, infrastructure;
    for (const auto& r : requests) {
        (r.find("POST /api/v1/custom_metrics") == 0 ? custom : infrastructure) = r;
    }
    ASSERT_TRUE(!custom.empty() && !infrastructure.empty());
    ASSERT_TRUE(infrastructure.find("POST /api/v1/infrastructure_metrics") == 0);
    ASSERT_TRUE(custom.find("Authorization: Bearer the-api-key") != std::string::npos || custom.find("authorization: Bearer the-api-key") != std::string::npos);
    nlohmann::json custom_body = nlohmann::json::parse(custom.substr(custom.find("\r\n\r\n") + 4));
    ASSERT_TRUE(custom_body["metrics"][0]["metric_name"] == "signup" && custom_body["metrics"][0]["value"] == 1.0);
    ASSERT_TRUE(custom_body["metrics"][1]["value"] == 49.0);
    ASSERT_TRUE(custom_body["metrics"][0]["release"] == "a1b2c3d");
    nlohmann::json infra_body = nlohmann::json::parse(infrastructure.substr(infrastructure.find("\r\n\r\n") + 4));
    ASSERT_TRUE(infra_body["metrics"][0]["hostname"] == "db-1");
    ASSERT_TRUE(infra_body["metrics"][1]["hostname"] == "web-1");
    forge_ops_tracker::reset_for_testing();
}

TEST(metrics_captures_are_a_no_op_when_reporting_is_not_enabled_for_this_environment) {
    TestServer server(202);
    tracker_init_for(server, [](Configuration& c) { c.environment = "development"; });
    forge_ops_tracker::capture_metric("signup");
    forge_ops_tracker::capture_infrastructure_metric("cpu", 1.0);
    forge_ops_tracker::flush_metrics();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(server.request_count() == 0);
    forge_ops_tracker::reset_for_testing();
}

TEST(metrics_uris_swap_the_trailing_events_segment) {
    Configuration config = metrics_configuration(1);
    ASSERT_TRUE(*config.custom_metrics_uri() == "http://127.0.0.1:1/api/v1/custom_metrics");
    ASSERT_TRUE(*config.infrastructure_metrics_uri() == "http://127.0.0.1:1/api/v1/infrastructure_metrics");
}


/* ---- SQL capture ---------------------------------------------------------------------------- */

TEST(sql_mask_replaces_strings_and_numbers_but_not_identifiers_or_placeholders) {
    using forge_ops_tracker::sql_statement::mask;
    ASSERT_TRUE(*mask("SELECT * FROM orders2 WHERE email = 'a@b.co' AND id = 42 AND x = $1") == "SELECT * FROM orders2 WHERE email = ? AND id = ? AND x = $1");
    ASSERT_TRUE(*mask("SELECT price * 1.5 FROM t WHERE a IN (1,2,3)") == "SELECT price * ? FROM t WHERE a IN (?,?,?)");
    ASSERT_TRUE(*mask("SELECT 1.5x FROM t") == "SELECT ?.5x FROM t");
}

TEST(sql_mask_handles_an_escaped_quote_a_cut_off_string_and_a_dollar_quoted_body) {
    using forge_ops_tracker::sql_statement::mask;
    ASSERT_TRUE(*mask("EXEC sp_x @t = 'it''s'") == "EXEC sp_x @t = ?");
    ASSERT_TRUE(*mask("SELECT 1 WHERE n = 'oops") == "SELECT ? WHERE n = ?");
    ASSERT_TRUE(*mask("DO $b$ BEGIN PERFORM 1; END $b$") == "DO ?");
}

TEST(sql_mask_is_idempotent_truncates_and_returns_nothing_for_blank) {
    using forge_ops_tracker::sql_statement::mask;
    std::string once = *mask("SELECT * FROM t WHERE a = 'x' AND b = 9");
    ASSERT_TRUE(*mask(once) == once);
    std::string long_sql = "SELECT ";
    for (int i = 0; i < 3000; ++i) long_sql += "a, ";
    ASSERT_TRUE(mask(long_sql + " b")->size() == forge_ops_tracker::sql_statement::kMaxLength + 3);
    ASSERT_TRUE(!mask("  ").has_value());
}

TEST(sql_extract_finds_a_stored_procedure_with_its_schema) {
    using forge_ops_tracker::sql_statement::extract_objects;
    auto found = extract_objects("EXEC dbo.sp_refund_order @id = ?");
    ASSERT_TRUE(found.has_value());
    ASSERT_TRUE((*found)["operation"] == "EXEC");
    ASSERT_TRUE((*found)["procedures"] == nlohmann::json::array({"dbo.sp_refund_order"}));
    ASSERT_TRUE((*found)["relations"] == nlohmann::json::array());
    ASSERT_TRUE((*extract_objects("CALL refund_order(?, ?)"))["procedures"] == nlohmann::json::array({"refund_order"}));
    ASSERT_TRUE((*extract_objects("SELECT refund_order(?, ?)"))["procedures"] == nlohmann::json::array({"refund_order"}));
}

TEST(sql_extract_finds_views_joined_tables_and_table_functions) {
    using forge_ops_tracker::sql_statement::extract_objects;
    ASSERT_TRUE((*extract_objects("SELECT * FROM v_totals t JOIN public.customers c ON c.id = t.id"))["relations"] == nlohmann::json::array({"v_totals", "public.customers"}));
    ASSERT_TRUE((*extract_objects("SELECT * FROM get_open_orders(?) o"))["procedures"] == nlohmann::json::array({"get_open_orders"}));
    ASSERT_TRUE((*extract_objects("UPDATE \"Order Items\" SET qty = ?"))["relations"] == nlohmann::json::array({"\"Order Items\""}));
    ASSERT_TRUE((*extract_objects("INSERT INTO [dbo].[audit_log] (a) VALUES (?)"))["relations"] == nlohmann::json::array({"[dbo].[audit_log]"}));
}

TEST(sql_extract_does_not_misread_column_lists_builtins_or_from_inside_extract) {
    using forge_ops_tracker::sql_statement::extract_objects;
    ASSERT_TRUE((*extract_objects("INSERT INTO audit_log (a) VALUES (?)"))["procedures"] == nlohmann::json::array());
    ASSERT_TRUE((*extract_objects("SELECT count(*) FROM orders"))["procedures"] == nlohmann::json::array());
    ASSERT_TRUE((*extract_objects("SELECT 1 FROM orders WHERE extract(year FROM created_at) = ?"))["relations"] == nlohmann::json::array({"orders"}));
    ASSERT_TRUE(!extract_objects("garbage").has_value());
}

TEST(sql_find_in_reads_a_sql_exception_a_nested_one_and_sqlite_text) {
    using forge_ops_tracker::SqlException;
    using forge_ops_tracker::sql_statement::find_in;
    ASSERT_TRUE(find_in(SqlException("boom", "SELECT 1")) == "SELECT 1");
    ASSERT_TRUE(find_in(std::runtime_error("no such table: t (code 1 SQLITE_ERROR): , while compiling: SELECT * FROM t")) == "SELECT * FROM t");
    ASSERT_TRUE(find_in(std::runtime_error("plain")).empty());

    std::string nested_result;
    try {
        try {
            throw SqlException("db", "CALL x(1)");
        } catch (...) {
            std::throw_with_nested(std::runtime_error("refund failed"));
        }
    } catch (const std::exception& e) {
        nested_result = find_in(e);
    }
    ASSERT_TRUE(nested_result == "CALL x(1)");
}

TEST(sql_event_builder_sends_the_procedure_name_by_default_and_the_statement_only_when_opted_in) {
    Configuration config;
    config.environment = "production";
    EventBuilder builder(config);
    const std::string sql = "EXEC dbo.sp_refund_order @order_id = 8814, @note = 'a@b.co'";

    nlohmann::json payload;
    try {
        throw forge_ops_tracker::SqlException("boom", sql);
    } catch (const std::exception& e) {
        payload = builder.build(e);
    }
    ASSERT_TRUE(payload["sql_objects"]["procedures"] == nlohmann::json::array({"dbo.sp_refund_order"}));
    ASSERT_TRUE(!payload.contains("sql_statement"));

    config.capture_sql_statement = true;
    try {
        throw fot_test_exc::BoomError("boom");
    } catch (const std::exception& e) {
        payload = builder.build(e, nlohmann::json::object(), nlohmann::json::object(), nlohmann::json::array(), sql);
    }
    ASSERT_TRUE(payload["sql_statement"] == "EXEC dbo.sp_refund_order @order_id = ?, @note = ?");
    ASSERT_TRUE(payload["exception_class"] == "fot_test_exc::BoomError");

    config.capture_sql_objects = false;
    config.capture_sql_statement = false;
    try {
        throw forge_ops_tracker::SqlException("boom", sql);
    } catch (const std::exception& e) {
        payload = builder.build(e);
    }
    ASSERT_TRUE(!payload.contains("sql_objects") && !payload.contains("sql_statement"));

    config.capture_sql_objects = true;
    try {
        throw fot_test_exc::BoomError("boom");
    } catch (const std::exception& e) {
        payload = builder.build(e);
    }
    ASSERT_TRUE(!payload.contains("sql_objects"));
}

int main() {
    RUN(configuration_defaults);
    RUN(configuration_api_key_and_ingestion_uri);
    RUN(configuration_api_key_percent_decodes);
    RUN(configuration_empty_or_malformed_dsn);
    RUN(configuration_dsn_with_no_userinfo);
    RUN(configuration_is_enabled);
    RUN(configuration_log_invokes_logger_and_no_ops_without_one);

    RUN(pii_scrub_email);
    RUN(pii_scrub_ssn);
    RUN(pii_scrub_credit_card);
    RUN(pii_leaves_ordinary_numeric_id_alone);
    RUN(pii_scrub_known_token_formats);
    RUN(pii_sensitive_key_scrub_ignores_case_and_punctuation);
    RUN(pii_scrub_recurses_through_nested_objects_and_arrays);
    RUN(pii_scrub_null_under_sensitive_key_stays_null);

    RUN(event_builder_basic_fields);
    RUN(event_builder_release_and_server_name_are_null_when_unset);
    RUN(event_builder_scrubs_message_and_context_by_default);
    RUN(event_builder_leaves_payload_untouched_when_scrub_pii_disabled);
    RUN(event_builder_includes_the_user_when_given_one_never_scrubbed_even_though_its_an_email);
    RUN(event_builder_includes_breadcrumbs_when_given);
    RUN(event_builder_omits_the_breadcrumbs_key_entirely_when_none_were_given);
    RUN(event_builder_scrubs_breadcrumb_message_and_data_but_not_category_level_or_timestamp);
    RUN(event_builder_omits_the_user_key_entirely_when_none_was_given);
    RUN(event_builder_non_std_exception_has_no_class_or_message);
    RUN(event_builder_backtrace_frames_are_well_formed_and_in_app_for_this_binary);
    RUN(event_builder_source_context_is_a_documented_noop);

    RUN(client_delivers_on_2xx_response);
    RUN(client_returns_false_on_non_2xx_response);
    RUN(client_returns_false_with_no_dsn);
    RUN(client_returns_false_when_unreachable);
    RUN(client_sends_authorization_bearer_header_and_json_body);

    RUN(delivery_queue_push_delivers_via_background_thread);
    RUN(delivery_queue_drops_once_full);
    RUN(delivery_queue_destructor_drains_pending_items_before_returning);

    RUN(reporter_report_does_nothing_when_disabled);
    RUN(reporter_report_delivers_when_enabled);
    RUN(reporter_report_includes_the_given_user_never_scrubbed_even_though_its_an_email);
    RUN(reporter_report_includes_the_given_breadcrumbs_in_the_delivered_payload);
    RUN(reporter_report_never_throws_for_a_non_std_exception);

    RUN(tracker_init_and_capture_exception_deliver_through_the_full_stack);
    RUN(tracker_set_user_attaches_the_user_to_a_later_capture_exception_call);
    RUN(tracker_an_explicit_user_argument_overrides_whatever_set_user_last_set);
    RUN(tracker_reset_for_testing_clears_the_current_user);
    RUN(tracker_add_breadcrumb_attaches_the_trail_to_a_later_capture_exception_call);
    RUN(tracker_add_breadcrumb_defaults_to_the_custom_category_and_info_level);
    RUN(tracker_add_breadcrumb_does_nothing_when_track_breadcrumbs_is_off);
    RUN(tracker_add_breadcrumb_caps_the_trail_at_max_breadcrumbs_dropping_the_oldest_first);
    RUN(tracker_clear_breadcrumbs_empties_the_trail);
    RUN(tracker_breadcrumbs_are_isolated_per_thread);
    RUN(tracker_reset_for_testing_clears_the_breadcrumb_trail);

    RUN(performance_record_buckets_by_transaction_name_with_count_sum_and_max);
    RUN(performance_record_does_nothing_when_track_performance_is_off_or_the_environment_is_not_enabled);
    RUN(performance_flush_delivers_one_batch_to_performance_samples_and_empties_the_buckets);
    RUN(performance_flush_does_nothing_when_there_is_nothing_to_send);
    RUN(performance_a_failed_delivery_keeps_every_bucket_so_the_next_flush_carries_more);
    RUN(performance_a_record_that_lands_during_delivery_is_never_lost);
    RUN(histogram_bucket_for_returns_the_smallest_boundary_a_duration_fits_under_as_a_string);
    RUN(histogram_bucket_for_returns_inf_for_anything_larger_than_the_largest_boundary);
    RUN(histogram_bucket_for_puts_a_duration_exactly_on_a_boundary_into_that_boundarys_own_bucket);
    RUN(histogram_boundaries_match_the_servers_histogram_percentile);
    RUN(performance_flush_delivers_a_latency_histogram_alongside_count_sum_and_max);
    RUN(performance_a_failed_delivery_keeps_histogram_counts_for_the_next_flush);
    RUN(performance_a_histogram_count_recorded_during_delivery_is_sent_on_the_next_flush);
    RUN(performance_the_background_thread_flushes_on_its_own_interval);
    RUN(performance_destroying_a_flusher_delivers_whatever_is_left_but_discard_does_not);
    RUN(performance_samples_uri_swaps_the_trailing_events_segment);
    RUN(tracker_record_performance_and_flush_performance_deliver_through_the_full_stack);
    RUN(tracker_time_transaction_returns_the_callables_value_and_records_how_long_it_took);
    RUN(tracker_scoped_transaction_records_even_when_the_scope_throws);
    RUN(tracker_track_performance_off_records_and_delivers_nothing);
    RUN(tracker_install_terminate_handler_is_idempotent);
    RUN(sql_mask_replaces_strings_and_numbers_but_not_identifiers_or_placeholders);
    RUN(sql_mask_handles_an_escaped_quote_a_cut_off_string_and_a_dollar_quoted_body);
    RUN(sql_mask_is_idempotent_truncates_and_returns_nothing_for_blank);
    RUN(sql_extract_finds_a_stored_procedure_with_its_schema);
    RUN(sql_extract_finds_views_joined_tables_and_table_functions);
    RUN(sql_extract_does_not_misread_column_lists_builtins_or_from_inside_extract);
    RUN(sql_find_in_reads_a_sql_exception_a_nested_one_and_sqlite_text);
    RUN(sql_event_builder_sends_the_procedure_name_by_default_and_the_statement_only_when_opted_in);


    RUN(tracing_span_buffer_nests_spans_under_the_open_one_and_the_root_with_the_wire_shape);
    RUN(tracing_span_buffer_sends_an_unknown_kind_as_other_since_the_server_would_reject_the_whole_trace);
    RUN(tracing_span_buffer_drops_a_trace_under_the_threshold_and_caps_a_big_one_at_500_spans);
    RUN(tracing_a_slow_scoped_trace_is_delivered_to_spans_with_nested_spans);
    RUN(tracing_a_scope_that_throws_is_still_recorded_and_sent);
    RUN(tracing_a_fast_trace_sends_nothing);
    RUN(tracing_track_tracing_off_or_reporting_disabled_records_and_sends_nothing);
    RUN(tracing_a_scoped_trace_nested_inside_a_trace_records_a_span_instead_of_starting_a_second_trace);
    RUN(tracing_trace_and_span_return_the_callables_value_and_a_span_outside_a_trace_just_runs);
    RUN(tracing_the_open_trace_is_per_thread);
    RUN(tracing_spans_uri_swaps_the_trailing_events_segment);

    RUN(traceparent_parses_a_valid_version_00_header);
    RUN(traceparent_rejects_anything_malformed);
    RUN(traceparent_accepts_a_future_version_with_extra_fields);
    RUN(traceparent_builds_a_sampled_version_00_header_and_ids_are_lowercase_hex_never_all_zeros);
    RUN(traceparent_url_host_extracts_just_the_lowercased_host);
    RUN(trace_propagation_goes_to_every_host_by_default_and_nowhere_when_off);
    RUN(trace_propagation_targets_match_hosts_on_a_dot_boundary_and_search_regexes_in_the_host);
    RUN(trace_context_span_buffer_continues_an_incoming_trace_and_never_sends_with_track_tracing_off);
    RUN(trace_context_a_continued_trace_is_sent_under_the_callers_span_and_the_http_span_names_itself_in_the_header);
    RUN(trace_context_a_missing_or_malformed_traceparent_starts_a_fresh_trace);
    RUN(trace_context_no_header_outside_a_trace_off_target_or_with_propagation_off);
    RUN(trace_context_an_error_captured_inside_a_trace_carries_its_id_even_with_track_tracing_off);
    RUN(trace_context_event_builder_attaches_trace_id_unscrubbed_and_omits_it_otherwise);


    RUN(metrics_flush_delivers_every_entry_as_one_batch_to_custom_metrics_with_the_wire_shape);
    RUN(metrics_a_nan_or_infinite_or_non_numeric_value_is_dropped);
    RUN(metrics_a_failed_delivery_keeps_every_entry_so_the_next_flush_carries_more);
    RUN(metrics_an_entry_recorded_while_delivery_is_in_flight_is_never_lost);
    RUN(metrics_a_buffer_is_capped_and_drops_further_entries_until_a_flush_succeeds);
    RUN(metrics_the_background_thread_flushes_on_its_own_interval);
    RUN(metrics_destroying_a_buffer_delivers_whatever_is_left_but_discard_does_not);
    RUN(metrics_capture_metric_and_capture_infrastructure_metric_deliver_to_their_own_endpoints_through_the_full_stack);
    RUN(metrics_captures_are_a_no_op_when_reporting_is_not_enabled_for_this_environment);
    RUN(metrics_uris_swap_the_trailing_events_segment);

    std::printf("\n%d run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
