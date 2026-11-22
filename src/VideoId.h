#pragma once
#include <algorithm>
#include <string_view>

namespace bilibili {
template<class Character>
bool validVideoId(std::basic_string_view<Character> id) {
    const auto prefix = id.substr(0, 2);
    if (id.size() == 12 && prefix[0] == 'B' && prefix[1] == 'V') {
        return std::all_of(id.begin() + 2, id.end(), [](Character c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        });
    }
    return id.size() >= 3 && id.size() <= 21 && prefix[0] == 'A' && prefix[1] == 'V' &&
        id[2] != '0' && std::all_of(id.begin() + 2, id.end(), [](Character c) { return c >= '0' && c <= '9'; });
}
}
