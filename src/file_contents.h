#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "optional.h"
#include "position.h"

struct FileContents {
    FileContents();
    FileContents(const AbsolutePath& path, const std::string& content);

    optional<int> ToOffset(Position p) const;
    optional<std::string> ContentsInRange(Range range) const;

    AbsolutePath path;
    std::string content;
    // {0, 1 + position of first newline, 1 + position of second newline, ...}
    std::vector<int> m_line_offsets;
};
