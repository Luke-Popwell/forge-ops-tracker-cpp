#include "forge_ops_tracker/histogram_bucketer.hpp"

namespace forge_ops_tracker {

std::string histogram_bucket_for(double duration_ms) {
    for (unsigned long boundary : kHistogramBoundariesMs) {
        if (duration_ms <= static_cast<double>(boundary)) {
            return std::to_string(boundary);
        }
    }
    return "inf";
}

} // namespace forge_ops_tracker
