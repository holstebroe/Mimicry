#include "host/ParamUtil.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace mimicry::host {

namespace {
std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

// Text after the last digit, if the text contains a digit at all.
std::string suffix_after_number(const std::string& text) {
    std::size_t last = std::string::npos;
    for (std::size_t i = 0; i < text.size(); ++i)
        if (std::isdigit(static_cast<unsigned char>(text[i]))) last = i;
    if (last == std::string::npos) return {};
    return trim(text.substr(last + 1));
}
}  // namespace

std::string units_from_texts(std::span<const std::string> texts) {
    std::string common;
    bool have = false;
    for (const auto& t : texts) {
        if (t.empty()) continue;
        const std::string s = suffix_after_number(t);
        if (s.empty() || s.size() > 8) return {};
        for (const unsigned char c : s)
            if (!std::isalpha(c) && c != '%' && c != '/') return {};
        if (!have) {
            common = s;
            have = true;
        } else if (common != s) {
            return {};
        }
    }
    return common;
}

bool looks_like_program_param(const std::string& name) {
    std::string n = trim(name);
    std::ranges::transform(n, n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return n == "program" || n == "preset" || n == "patch" || n == "program change";
}

bool use_log_scale(double min, double max, int steps, const std::string& units) {
    if (steps > 0 || !(min > 0) || !(max > min)) return false;
    if (units == "Hz" || units == "kHz" || units == "ms" || units == "s") return true;
    return max / min >= 20.0;
}

double coord_to_plain(const ParamDesc& p, double u) {
    u = std::clamp(u, 0.0, 1.0);
    double plain = p.log_scale ? p.min * std::pow(p.max / p.min, u) : p.min + u * (p.max - p.min);
    if (p.steps > 1) {
        const double span = p.max - p.min;
        const double step = span / static_cast<double>(p.steps - 1);
        plain = p.min + std::round((plain - p.min) / step) * step;
    }
    return std::clamp(plain, p.min, p.max);
}

double plain_to_coord(const ParamDesc& p, double plain) {
    if (!(p.max > p.min)) return 0.0;
    plain = std::clamp(plain, p.min, p.max);
    const double u = p.log_scale ? std::log(plain / p.min) / std::log(p.max / p.min)
                                 : (plain - p.min) / (p.max - p.min);
    return std::clamp(u, 0.0, 1.0);
}

}  // namespace mimicry::host
