#pragma once

#include <cmath>
#include <iomanip>
#include <locale>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fah {

// Empty text and a lone dot are valid intermediate editing states.
inline bool decimalPartial(std::string_view text) {
    bool dot = false;
    for (char character : text) {
        if (character >= '0' && character <= '9') continue;
        if (character != '.' || dot) return false;
        dot = true;
    }
    return true;
}

// Preserve finite, non-negative values without putting exponent notation into
// an edit control that accepts only digits and a decimal point.
inline std::string decimalText(double value) {
    if (!std::isfinite(value) || value < 0)
        throw std::runtime_error("Decimal value must be finite and non-negative");
    if (value == 0) return "0";
    auto format = [&](int precision) {
        std::ostringstream output;
        output.imbue(std::locale::classic());
        output << std::setprecision(precision) << value;
        return output.str();
    };
    std::string text = format(15);
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    double recovered = 0;
    const bool parsed = static_cast<bool>(input >> recovered);
    // Some standard libraries set failbit for a correctly recovered subnormal.
    const bool subnormal = value < std::numeric_limits<double>::min() && recovered > 0;
    if ((!parsed && !subnormal) || recovered != value) text = format(17);
    const auto exponentAt = text.find_first_of("eE");
    if (exponentAt == std::string::npos) return text;
    const int exponent = std::stoi(text.substr(exponentAt + 1));
    text.resize(exponentAt);
    const auto dotAt = text.find('.');
    int decimal = static_cast<int>(dotAt == std::string::npos ? text.size() : dotAt) + exponent;
    if (dotAt != std::string::npos) text.erase(dotAt, 1);
    if (decimal <= 0) return "0." + std::string(static_cast<std::size_t>(-decimal), '0') + text;
    if (decimal >= static_cast<int>(text.size()))
        return text + std::string(static_cast<std::size_t>(decimal) - text.size(), '0');
    text.insert(static_cast<std::size_t>(decimal), 1, '.');
    return text;
}

} // namespace fah
