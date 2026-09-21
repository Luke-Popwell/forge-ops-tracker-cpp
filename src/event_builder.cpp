#include "forge_ops_tracker/event_builder.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <cxxabi.h>
#include <execinfo.h>
#include <iomanip>
#include <regex>
#include <sstream>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "forge_ops_tracker/pii_scrubber.hpp"
#include "forge_ops_tracker/sql_statement.hpp"

namespace forge_ops_tracker {

namespace {

constexpr std::size_t kMaxFrames = 128; // backtrace() itself has no unbounded-growth risk the way a manually-built array does, so this is a generous cap, not a hard safety limit

// Would be the source-context window size and per-line truncation length (see other SDKs'
// EventBuilder for the real version of this), if this SDK's backtrace() ever produced a real
// file+line pair to key a disk read off of. It doesn't: see EventBuilder.hpp's own header
// comment: so these exist unused here purely as a documented placeholder, kept named
// consistently with kMaxFrames above, rather than silently having no trace of the concept at all.
[[maybe_unused]] constexpr std::size_t kContextLines = 5;
[[maybe_unused]] constexpr std::size_t kMaxContextLineLength = 500;

// Identifies this client to the server's auto language-detection on the project the event lands
// in (see Project#note_sdk_platform server-side); matches this repo's own sdks/cpp directory
// name, the same convention every other language's client follows.
constexpr const char* kSdkName = "cpp";

std::string demangle(const char* mangled) {
    int status = 0;
    char* demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    if (status == 0 && demangled != nullptr) {
        std::string result(demangled);
        std::free(demangled);
        return result;
    }
    return mangled;
}

std::string exception_class_name(const std::exception& e) {
    return demangle(typeid(e).name());
}

std::string current_executable_name() {
#if defined(__APPLE__)
    char path[4096];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) {
        return "";
    }
    std::string full(path);
    auto slash = full.find_last_of('/');
    return slash == std::string::npos ? full : full.substr(slash + 1);
#else
    return "";
#endif
}

// e.g. "12  MyApp    0x0000000100abcd12 _ZN3foo3barEv + 82": frame index, image name, address,
// symbol, "+ offset". Same shape backtrace_symbols() produces on both macOS and glibc-based Linux
// (both ultimately implement the same de facto convention): verified directly against real
// backtrace_symbols() output from a real caught exception before relying on it.
const std::regex& frame_pattern() {
    static const std::regex pattern(R"(^\s*\d+\s+(\S+)\s+0x[0-9a-fA-F]+\s+(.+?)\s+\+\s+\d+\s*$)");
    return pattern;
}

} // namespace

EventBuilder::EventBuilder(const Configuration& configuration) : configuration_(configuration) {}

nlohmann::json EventBuilder::build(const std::exception_ptr& exception_ptr, const nlohmann::json& context, const nlohmann::json& user, const nlohmann::json& breadcrumbs, const std::string& sql) {
    std::string exception_class = "unknown exception";
    std::string message;
    std::string raw_statement = sql;

    try {
        std::rethrow_exception(exception_ptr);
    } catch (const std::exception& e) {
        exception_class = exception_class_name(e);
        message = e.what();
        if (raw_statement.empty()) {
            raw_statement = sql_statement::find_in(e);
        }
    } catch (...) {
        // A non-std::exception throw: any type at all can be thrown in C++. There's genuinely
        // no name or message available for this case; exception_class stays "unknown exception"
        // rather than guessing.
    }

    nlohmann::json payload = {
        {"exception_class", exception_class},
        {"message", message},
        {"backtrace", backtrace()},
        {"occurred_at", [] {
            auto now = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            std::ostringstream out;
            out << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
            return out.str();
        }()},
        {"environment", configuration_.environment},
        {"release", configuration_.release ? nlohmann::json(*configuration_.release) : nlohmann::json(nullptr)},
        {"server_name", configuration_.server_name ? nlohmann::json(*configuration_.server_name) : nlohmann::json(nullptr)},
        {"context", context},
        {"tags", nlohmann::json::object()},
        {"sdk_name", kSdkName},
    };
    // Omitted entirely (never sent as an empty array) when there's nothing to report; part of the
    // payload scrub_payload sees, so message/data get scrubbed with everything else.
    if (!breadcrumbs.empty()) {
        payload["breadcrumbs"] = breadcrumbs;
    }
    attach_sql(payload, raw_statement);

    nlohmann::json built = configuration_.scrub_pii ? scrub_payload(payload) : payload;

    // Attached after scrubbing, never passed through it: a structured field the host app sets
    // deliberately, not free text that could accidentally spill sensitive data, the same
    // exemption exception_class/environment/release/server_name already get above. Omitted from
    // the wire entirely (not even an empty object) when empty, matching every other client in
    // this repo's own "user" field.
    if (!user.empty()) {
        built["user"] = user;
    }
    return built;
}

nlohmann::json EventBuilder::build(const std::exception& exception, const nlohmann::json& context, const nlohmann::json& user, const nlohmann::json& breadcrumbs, const std::string& sql) {
    // std::make_exception_ptr(exception) looks right here but silently slices: template argument
    // deduction picks E from `exception`'s declared type (std::exception&, this parameter's own
    // static type), not its dynamic type, so a derived exception like a caught std::runtime_error
    // subclass comes back through the rebuilt exception_ptr as a bare std::exception: its real
    // class name and its own what() message both lost, replaced with std::exception's generic
    // boilerplate. Verified directly: a caught custom exception with a real message round-tripped
    // through make_exception_ptr(exception) here came back as exception_class "std::exception" and
    // message "std::exception", not the original type/text, before this fix.
    //
    // std::current_exception() has no such problem: it reflects whatever's genuinely in flight,
    // which is exactly this overload's own documented use case ("already having caught a
    // std::exception"): calling this from inside that same catch block, even through another
    // function call, still counts as "being handled" per the standard, so the exception_ptr it
    // returns carries the real dynamic type. Falls back to make_exception_ptr only when nothing is
    // actually in flight (a caller building/reporting a std::exception object it never threw):
    // that path genuinely can't recover a derived type through a plain base-class reference; no
    // function can work around that, it's a hard C++ limitation, not a bug in this one.
    std::exception_ptr current = std::current_exception();
    return build(current ? current : std::make_exception_ptr(exception), context, user, breadcrumbs, sql);
}

// See sql_statement.hpp for how the statement is masked. The statement itself only goes out when
// capture_sql_statement is on; the extracted names go out on their own (capture_sql_objects) so an
// issue can still name the procedure or view involved.
void EventBuilder::attach_sql(nlohmann::json& payload, const std::string& raw_statement) const {
    if (!configuration_.capture_sql_objects && !configuration_.capture_sql_statement) {
        return;
    }

    std::optional<std::string> masked = sql_statement::mask(raw_statement);
    if (!masked) {
        return;
    }

    if (configuration_.capture_sql_objects) {
        if (std::optional<nlohmann::json> objects = sql_statement::extract_objects(*masked)) {
            payload["sql_objects"] = *objects;
        }
    }
    if (configuration_.capture_sql_statement) {
        payload["sql_statement"] = *masked;
    }
}

nlohmann::json EventBuilder::backtrace() const {
    void* addresses[kMaxFrames];
    int count = ::backtrace(addresses, static_cast<int>(kMaxFrames));
    char** symbols = ::backtrace_symbols(addresses, count);

    nlohmann::json frames = nlohmann::json::array();
    if (symbols == nullptr) {
        return frames;
    }

    for (int i = 0; i < count; ++i) {
        std::string line(symbols[i]);
        std::smatch match;
        if (!std::regex_match(line, match, frame_pattern())) {
            continue; // an unparseable line is skipped, not an error: see PHP's own client for the same philosophy
        }

        std::string image = match[1].str();
        std::string symbol = demangle(match[2].str().c_str());

        frames.push_back(attach_source_context({
            {"file", image},
            {"line", nullptr}, // no line-level info at runtime: see this class's own header comment
            {"method", symbol},
            {"in_app", is_in_app(image)},
        }));
    }

    std::free(symbols);
    return frames;
}

nlohmann::json EventBuilder::attach_source_context(nlohmann::json frame) const {
    // Deliberately a no-op. The gating logic every other SDK applies here is: the config option is
    // on, AND the frame is in-app, AND a real file path + line number is actually available. The
    // third condition can never be true on this SDK's own capture path: backtrace()/
    // backtrace_symbols() give a binary image name and a resolved symbol, never a source file or a
    // line number (frame["line"] above is always JSON null), so there is nothing configuration_.
    // capture_source_context could ever gate here even though it exists (see configuration.hpp's
    // own comment) for API-shape consistency with every other SDK. No disk read is ever attempted,
    // regardless of what that flag is set to.
    return frame;
}

bool EventBuilder::is_in_app(const std::string& image) const {
    std::string executable = current_executable_name();
    return !executable.empty() && image == executable;
}

nlohmann::json EventBuilder::scrub_payload(nlohmann::json payload) const {
    payload["message"] = pii_scrubber::scrub_string(payload["message"].get<std::string>());
    if (payload.contains("sql_statement")) {
        payload["sql_statement"] = pii_scrubber::scrub_string(payload["sql_statement"].get<std::string>());
    }
    for (auto& frame : payload["backtrace"]) {
        if (frame["file"].is_string()) {
            frame["file"] = pii_scrubber::scrub_string(frame["file"].get<std::string>());
        }
        if (frame["method"].is_string()) {
            frame["method"] = pii_scrubber::scrub_string(frame["method"].get<std::string>());
        }
    }
    // Breadcrumb message/data are free text the host app wrote; category, level, and timestamp
    // are structured values set deliberately, so they are left alone, the same split as the
    // top-level fields above.
    if (payload.contains("breadcrumbs")) {
        for (auto& crumb : payload["breadcrumbs"]) {
            if (crumb["message"].is_string()) {
                crumb["message"] = pii_scrubber::scrub_string(crumb["message"].get<std::string>());
            }
            crumb["data"] = pii_scrubber::scrub(crumb["data"]);
        }
    }
    payload["context"] = pii_scrubber::scrub(payload["context"]);
    payload["tags"] = pii_scrubber::scrub(payload["tags"]);
    return payload;
}

} // namespace forge_ops_tracker
