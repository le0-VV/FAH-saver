#pragma once

#include <fah/DecimalInput.hpp>
#include <string>
#include <string_view>

namespace fah::windows {

inline bool decimalReplacement(std::wstring_view current, std::size_t first,
                               std::size_t last, std::wstring_view insertion) {
    if (first > last || last > current.size()) return false;
    std::wstring proposed(current);
    proposed.replace(first, last-first, insertion);
    std::string ascii;
    ascii.reserve(proposed.size());
    for (wchar_t character : proposed) {
        if (character > 127) return false;
        ascii.push_back(static_cast<char>(character));
    }
    return fah::decimalPartial(ascii);
}

} // namespace fah::windows
