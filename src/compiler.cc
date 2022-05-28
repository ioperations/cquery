#include "compiler.h"

#include <doctest/doctest.h>

#include <unordered_map>

#include "platform.h"
#include "utils.h"

namespace {
compiler_type ExtractCompilerType(const std::string& version_output) {
    if (version_output.find("Apple LLVM version") != std::string::npos)
        return compiler_type::Clang;
    if (version_output.find("clang version") != std::string::npos)
        return compiler_type::Clang;
    if (version_output.find("GCC") != std::string::npos)
        return compiler_type::GCC;
    if (version_output.find("Microsoft (R)") != std::string::npos)
        return compiler_type::MSVC;
    return compiler_type::Unknown;
}

// FIXME: Make FindCompilerType a class so this is not a global.
std::unordered_map<std::string, compiler_type> m_compiler_type_cache;

}  // namespace

compiler_type FindCompilerType(const std::string& compiler_driver) {
    auto it = m_compiler_type_cache.find(compiler_driver);
    if (it != m_compiler_type_cache.end()) return it->second;

    std::vector<std::string> flags = {compiler_driver};
    if (!EndsWith(compiler_driver, "cl.exe")) flags.push_back("--version");
    optional<std::string> version_output = RunExecutable(flags, "");
    compiler_type result = compiler_type::Unknown;
    if (version_output) result = ExtractCompilerType(version_output.value());

    m_compiler_type_cache[compiler_driver] = result;
    return result;
}

bool CompilerAcceptsFlag(compiler_type compiler_type, const std::string& flag) {
    // MSVC does not accept flag beginning with '-'.
    if (compiler_type == compiler_type::MSVC && StartsWith(flag, "-"))
        return false;

    // These flags are for clang only.
    if (StartsWith(flag, "-working-directory") ||
        StartsWith(flag, "-resource-dir") || flag == "-fparse-all-comments")
        return compiler_type == compiler_type::Clang;

    return true;
}

void CompilerAppendsFlagIfAccept(compiler_type compiler_type,
                                 const std::string& flag,
                                 std::vector<std::string>& flags) {
    if (CompilerAcceptsFlag(compiler_type, flag)) flags.emplace_back(flag);
}

TEST_SUITE("Compiler type extraction") {
    TEST_CASE("Apple Clang") {
        std::string version_output =
            "Apple LLVM version 9.1.0 (clang-902.0.39.1)\n"
            "Target: x86_64-apple-darwin17.5.0\n"
            "Thread model: posix\n"
            "InstalledDir: "
            "/Applications/Xcode.app/Contents/Developer/Toolchains/"
            "XcodeDefault.xctoolchain/usr/bin\n";
        REQUIRE(compiler_type::Clang == ExtractCompilerType(version_output));
    }
    TEST_CASE("LLVM Clang") {
        std::string version_output =
            "clang version 6.0.0 (tags/RELEASE_600/final)\n"
            "Target: x86_64-apple-darwin17.5.0\n"
            "Thread model: posix\n"
            "InstalledDir: /usr/local/opt/llvm/bin\n";
        REQUIRE(compiler_type::Clang == ExtractCompilerType(version_output));
    }
    TEST_CASE("GCC") {
        std::string version_output =
            "gcc-8 (Homebrew GCC 8.1.0) 8.1.0\n"
            "Copyright (C) 2018 Free Software Foundation, Inc.\n"
            "This is free software; see the source for copying conditions.  "
            "There "
            "is NO\n"
            "warranty; not even for MERCHANTABILITY or FITNESS FOR A "
            "PARTICULAR "
            "PURPOSE.\n";
        REQUIRE(compiler_type::GCC == ExtractCompilerType(version_output));
    }
    TEST_CASE("MSVC") {
        std::string version_output =
            "Microsoft (R) C/C++ Optimizing Compiler Version 19.00.24210 for "
            "x64\n"
            "Copyright (C) Microsoft Corporation.  All rights reserved.\n"
            "\n"
            "usage: cl [ option... ] filename... [ /link linkoption... ]\n";
        REQUIRE(compiler_type::MSVC == ExtractCompilerType(version_output));
    }
    TEST_CASE("Unknown") {
        std::string version_output = "";
        REQUIRE(compiler_type::Unknown == ExtractCompilerType(version_output));
    }
}
