#pragma once

#include <string>
#include <vector>

#include "lsp.h"

std::vector<LsTextEdit> RunClangFormat(const std::string& filename,
                                       const std::string& file_contents,
                                       optional<int> start_offset,
                                       optional<int> end_offset);
