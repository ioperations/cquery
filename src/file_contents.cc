#include "file_contents.h"

FileContents::FileContents() : m_line_offsets{0} {}

FileContents::FileContents(const AbsolutePath& path, const std::string& content)
    : path(path), content(content) {
    m_line_offsets.push_back(0);
    for (size_t i = 0; i < content.size(); i++) {
        if (content[i] == '\n') m_line_offsets.push_back(i + 1);
    }
}

optional<int> FileContents::ToOffset(Position p) const {
    if (0 <= p.line && size_t(p.line) < m_line_offsets.size()) {
        int ret = m_line_offsets[p.line] + p.column;
        if (size_t(ret) < content.size()) return ret;
    }
    return nullopt;
}

optional<std::string> FileContents::ContentsInRange(Range range) const {
    optional<int> start_offset = ToOffset(range.start),
                  end_offset = ToOffset(range.end);
    if (start_offset && end_offset && *start_offset < *end_offset)
        return content.substr(*start_offset, *end_offset - *start_offset);
    return nullopt;
}
