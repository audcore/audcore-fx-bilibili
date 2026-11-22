#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bilibili {
struct Target {
    std::wstring id;
    uint16_t part = 1;
    uint32_t seconds = 0;
};
struct Pixels {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bytes;
};
struct SearchRow {
    Target target;
    std::wstring title, author, duration, cover;
    std::shared_ptr<const Pixels> image;
};
struct SearchPage {
    int pages = 0;
    std::vector<SearchRow> rows;
};
struct Part {
    uint16_t page;
    std::wstring title;
};
struct Comment {
    double time;
    int mode;
    uint32_t color;
    std::wstring text;
};
struct Video {
    Target target;
    std::wstring title, media;
    std::vector<Part> parts;
    uint64_t cid = 0;
};

std::optional<Target> parseTarget(std::wstring_view text);
std::optional<SearchPage> search(const std::wstring& base, const std::wstring& query, int page);
std::shared_ptr<const Pixels> loadCover(const std::wstring& base, const std::wstring& cover);
std::optional<Video> loadVideo(const std::wstring& base, const Target& target);
std::vector<Comment> loadComments(const std::wstring& base, uint64_t cid);
}
