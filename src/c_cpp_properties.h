#pragma once

#include <string>
#include <vector>

#include "optional.h"

struct CCppProperties {
    std::string c_standard;
    std::string cpp_standard;
    std::vector<std::string> args;
};

optional<CCppProperties> LoadCCppProperties(const std::string& json_full_path,
                                            const std::string& project_dir);
