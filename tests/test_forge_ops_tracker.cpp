/*
 * A minimal, hand-rolled test runner rather than an external framework (Catch2/GoogleTest) --
 * mirrors sdks/c/tests/test_forgeops_tracker.c's own decision on purpose, not by accident. Two
 * reasons that decision carries over to C++ specifically: this SDK is closest in spirit and
 * toolchain to sdks/c (same libcurl/execinfo/POSIX-signal foundations, same "one platform-shaped
 * client, not a portable-everywhere one" scope), and a test binary is the one place in this repo
 * where reaching for a vendored dependency purely for the tests themselves, on top of the library's
 * own real runtime dependencies (libcurl, nlohmann_json -- see the README's "Dependencies"
 * section), would add a second dependency for no capability this file's own macros don't already
 * provide. C++ exceptions make the hand-rolled approach noticeably less painful than C's version --
 * RUN() below catches whatever a test throws instead of every ASSERT needing its own manual
 * unwind-and-return dance -- so there wasn't a real capability gap pulling toward Catch2 either.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
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
#include "forge_ops_tracker/pii_scrubber.hpp"
#include "forge_ops_tracker/reporter.hpp"

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
 * POST body -- confirmed there as a real bug in that test file's first draft, applied here from the
 * start instead of rediscovering it. Runs its accept loop on a background std::thread rather than a
 * forked process (no fork() available on every target this repo would eventually port to, and
 * nothing here needs the process isolation forking buys); poll()s with a short timeout so the
 * thread notices `stopping_` promptly instead of blocking in accept() forever. */
class TestServer {
public:
    /* status_code >= 0: a normal server that accepts, reads, and responds. status_code < 0: a
     * "black hole" -- the listening socket exists (so a DSN can point at its port) but nothing
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
 * "N13fot_test_exc9BoomErrorE" into "fot_test_exc::BoomError", no "class " prefix, no surprises --
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
     * same runtime value either way) rather than one contiguous literal -- none of these were ever
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
     * sensitive key when the value isn't already null -- redacting "null" to "[FILTERED]" would
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

TEST(event_builder_non_std_exception_has_no_class_or_message) {
    Configuration config;
    EventBuilder builder(config);

    std::exception_ptr ptr;
    try {
        throw 42; // not a std::exception at all -- legal in C++, and genuinely un-nameable
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
        ASSERT_TRUE(frame["line"].is_null()); // no line-level info from backtrace_symbols() -- see the header comment
        ASSERT_TRUE(frame["method"].is_string());
        ASSERT_TRUE(frame["in_app"].is_boolean());
        if (frame["in_app"] == true) {
            saw_in_app_frame = true;
        }
    }
    /* This test binary links forge_ops_tracker statically, so every frame that unwinds back into
     * this executable (at minimum the RUN() call site in main()) shares this binary's own image
     * name and should be classified in_app -- confirmed directly against real backtrace_symbols()
     * output on this machine before relying on it (see the header's is_in_app() comment for the
     * caveat this only works because nothing here is a separate shared library). */
    ASSERT_TRUE(saw_in_app_frame);
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
    config.dsn = "http://key@127.0.0.1:1/api/v1/events"; // port 1 -- nothing listens there
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
     * capacity -- see TestServer's own header comment for why status_code < 0 hangs instead of
     * responding. */
    TestServer server(-1);
    Configuration config;
    config.dsn = dsn_for(server);
    config.queue_size = 2;
    config.timeout_seconds = 1;
    std::vector<std::string> logged;
    config.logger = [&logged](const std::string& message) { logged.push_back(message); };

    DeliveryQueue queue(config, Client(config));

    ASSERT_TRUE(queue.push({{"n", 1}})); // picked up by the worker almost immediately, which then blocks on the black hole
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); // let the worker actually start blocking on it

    ASSERT_TRUE(queue.push({{"n", 2}})); // queue: [2] -- 1 of 2 slots used
    ASSERT_TRUE(queue.push({{"n", 3}})); // queue: [2, 3] -- at capacity
    ASSERT_TRUE(!queue.push({{"n", 4}})); // over capacity -- dropped

    bool logged_drop = false;
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
        // queue destructs here -- its own destructor sets stopping_ and joins the worker, and
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
    config.environment = "development"; // not in enabled_environments -- disabled
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

TEST(reporter_report_never_throws_for_a_non_std_exception) {
    Configuration config; // disabled (no dsn) -- this test only cares that report() itself never throws
    DeliveryQueue queue(config, Client(config));
    Reporter reporter(config, EventBuilder(config), queue);

    std::exception_ptr ptr;
    try {
        throw 42;
    } catch (...) {
        ptr = std::current_exception();
    }
    reporter.report(ptr); // must not throw -- RUN() would catch and fail this test if it did
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

TEST(tracker_install_terminate_handler_is_idempotent) {
    /* Must not crash (or double-chain) on a second call -- same property sdks/c's own
     * tracker_install_handlers_is_idempotent test checks for its own installer. */
    forge_ops_tracker::install_terminate_handler();
    forge_ops_tracker::install_terminate_handler();
}

/* ---- main ------------------------------------------------------------------------------------- */

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
    RUN(event_builder_non_std_exception_has_no_class_or_message);
    RUN(event_builder_backtrace_frames_are_well_formed_and_in_app_for_this_binary);

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
    RUN(reporter_report_never_throws_for_a_non_std_exception);

    RUN(tracker_init_and_capture_exception_deliver_through_the_full_stack);
    RUN(tracker_install_terminate_handler_is_idempotent);

    std::printf("\n%d run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
