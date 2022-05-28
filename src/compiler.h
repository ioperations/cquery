#pragma once

#include <string>
#include <vector>

// Used to identify the compiler type.
enum class compiler_type {
    Unknown = 0,
    Clang = 1,
    GCC = 2,
    MSVC = 3,
};

// Find out the compiler type for the specific driver.
compiler_type FindCompilerType(const std::string& compiler_driver);

// Whether the compiler accepts certain flag.
bool CompilerAcceptsFlag(compiler_type compiler_type, const std::string& flag);

// Append flag if the compiler accepts it.
void CompilerAppendsFlagIfAccept(compiler_type compiler_type,
                                 const std::string& flag,
                                 std::vector<std::string>& flags);
