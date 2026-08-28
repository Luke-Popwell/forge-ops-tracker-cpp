#pragma once

#include <nlohmann/json.hpp>

#include "forge_ops_tracker/configuration.hpp"

namespace forge_ops_tracker {

/**
 * Delivers one payload over HTTP. Every failure mode -- DNS, connection, timeout, a non-2xx
 * response -- is caught here and turned into a `false` return rather than a thrown exception,
 * since a broken or unreachable tracker must never be able to break the host app. Uses libcurl --
 * not a bespoke choice for this SDK specifically, but the closest C++ equivalent to "the
 * platform's own standard HTTP client" every other SDK in this repo reaches for (Ruby's
 * Net::HTTP, Python's urllib, PHP's curl extension already wrapping the same library under the
 * hood): there's no HTTP client in the C++ standard library itself, and libcurl is close to
 * universally available wherever a C++ toolchain already is.
 */
class Client {
public:
    explicit Client(const Configuration& configuration);

    bool deliver(const nlohmann::json& payload) const;

private:
    const Configuration& configuration_;
};

} // namespace forge_ops_tracker
