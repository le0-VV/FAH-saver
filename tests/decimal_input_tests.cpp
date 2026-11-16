#include "fah/DecimalInput.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void rejects(const std::function<void()>& action) {
    try { action(); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Invalid decimal value was formatted");
}
void roundtrip(double value) {
    const auto text = fah::decimalText(value);
    check(!text.empty() && fah::decimalPartial(text), "Formatter produced non-decimal edit text");
    // strtod can return a representable subnormal with ERANGE; the value must
    // still round-trip, rather than treating that status as a formatting error.
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    check(end == text.c_str() + text.size() && parsed == value, "Decimal text did not round-trip exactly");
}
class CommaPunctuation : public std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '_'; }
    std::string do_grouping() const override { return "\3"; }
};
} // namespace

int main() {
    try {
        for (const auto* text : {"", ".", ".5", "1.", "001", "0.05", "60.5", "100000000000000000000"})
            check(fah::decimalPartial(text), "A valid partial decimal was rejected");
        for (const auto* text : {"..", "1.2.3", "-1", "+1", " 1", "1 ", "1,5", "1e100", "NaN", "Inf", "1\n", "\xef\xbc\x91", "\xd9\xa1"})
            check(!fah::decimalPartial(text), "Invalid characters entered the decimal edit control");
        check(!fah::decimalPartial(std::string("1\0.5", 4)), "Embedded NUL was accepted");
        check(fah::decimalText(0) == "0" && fah::decimalText(-0.) == "0", "Zero was not normalised");
        check(fah::decimalText(.05) == "0.05" && fah::decimalText(.5) == "0.5" &&
              fah::decimalText(1) == "1" && fah::decimalText(60.5) == "60.5",
              "Ordinary values are not compact");
        check(fah::decimalText(1e100) == "1" + std::string(100, '0'), "Large exponent was not expanded");
        check(fah::decimalText(1e-309) == "0." + std::string(308, '0') + "1",
              "A round-tripping short subnormal was unnecessarily expanded to 17 significant digits");
        for (double value : {0., .05, .1, .5, 1., 60.5, 1000., 1e-10, 1e100,
                             1.23456789012345, 1.2345678901234567,
                             std::nextafter(1., 2.), std::nextafter(60., 61.),
                             std::numeric_limits<double>::min(), std::numeric_limits<double>::denorm_min(),
                             std::numeric_limits<double>::max()})
            roundtrip(value);
        rejects([] { fah::decimalText(-.05); });
        rejects([] { fah::decimalText(std::numeric_limits<double>::quiet_NaN()); });
        rejects([] { fah::decimalText(std::numeric_limits<double>::infinity()); });
        rejects([] { fah::decimalText(-std::numeric_limits<double>::infinity()); });
        const auto previous = std::locale();
        std::locale::global(std::locale(previous, new CommaPunctuation));
        const bool classic = fah::decimalText(1234.5) == "1234.5" && fah::decimalText(.05) == "0.05";
        std::locale::global(previous);
        check(classic, "Global C++ locale changed decimal edit text");
        std::cout << "Decimal partial-input and exact plain-decimal formatting checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
