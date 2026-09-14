// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Portable acceptance check for the active AArch64 exporter (arm64_to_c.h):
//   1. EmitProject on a tiny supported sequence, then CMake-compile the project
//   2. RET Rn / BLR X30 probes compiled and executed as permanent regressions
//
// Deliberately does not invoke tools/static_recompiler.

#include "core/recompiler/arm64_to_c.h"
#include "smoke_config.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <process.h>
#endif

namespace fs = std::filesystem;
using suyu::recomp::u32;
using suyu::recomp::u64;

namespace {

int g_fails = 0;

void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << std::endl;
    ++g_fails;
}

void pass(const std::string& msg) {
    std::cout << "PASS: " << msg << std::endl;
}

std::string Quote(const std::string& s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    return "'" + s + "'";
#endif
}

int RunArgs(const std::vector<std::string>& args) {
    if (args.empty()) {
        fail("empty command");
        return 1;
    }
    std::cout << '+';
    for (const auto& a : args) {
        std::cout << ' ' << Quote(a);
    }
    std::cout << std::endl;
#ifdef _WIN32
    // std::system() is cmd.exe /c, which strips the first/last quote when the
    // line has several quoted tokens — so "C:/Program Files/CMake/..." becomes
    // C:/Program. Spawn the argv list instead.
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);
    const intptr_t rc =
        _spawnv(_P_WAIT, args[0].c_str(), reinterpret_cast<const char* const*>(argv.data()));
    if (rc < 0) {
        fail(std::string("spawn ") + args[0] + ": " + std::strerror(errno));
        return 1;
    }
    return static_cast<int>(rc);
#else
    std::ostringstream cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) {
            cmd << ' ';
        }
        cmd << Quote(args[i]);
    }
    return std::system(cmd.str().c_str());
#endif
}

bool WriteFile(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        fail("write " + path.string());
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// AArch64 encodings used by the review's reproductions.
constexpr u32 kMovzX0_5 = 0xD28000A0u;
constexpr u32 kMovzX1_7 = 0xD28000E1u;
constexpr u32 kAddX2X0X1 = 0x8B010002u;
constexpr u32 kSvc0 = 0xD4000001u;
constexpr u32 kRetX5 = 0xD65F00A0u;
constexpr u32 kRetX30 = 0xD65F03C0u;
constexpr u32 kBlrX30 = 0xD63F03C0u;

bool AesHelpersAtFileScope(const std::string& runtime_c) {
    const auto save = runtime_c.find("int recomp_save_write(");
    const auto sbox = runtime_c.find("recomp_aes_sbox");
    if (save == std::string::npos) {
        fail("generated runtime missing recomp_save_write");
        return false;
    }
    if (sbox == std::string::npos) {
        fail("generated runtime missing recomp_aes_sbox");
        return false;
    }
    const auto brace = runtime_c.find('{', save);
    if (brace == std::string::npos) {
        fail("recomp_save_write has no body");
        return false;
    }
    int depth = 0;
    size_t end = std::string::npos;
    for (size_t i = brace; i < runtime_c.size(); ++i) {
        if (runtime_c[i] == '{') {
            ++depth;
        } else if (runtime_c[i] == '}') {
            --depth;
            if (depth == 0) {
                end = i;
                break;
            }
        }
    }
    if (end == std::string::npos) {
        fail("unterminated recomp_save_write");
        return false;
    }
    if (sbox > brace && sbox < end) {
        fail("recomp_aes_sbox is nested inside recomp_save_write");
        return false;
    }
    const auto gmul = runtime_c.find("recomp_gmul");
    if (gmul != std::string::npos && gmul > brace && gmul < end) {
        fail("recomp_gmul is nested inside recomp_save_write");
        return false;
    }
    pass("AES helpers are at file scope");
    return true;
}

int CmakeBuild(const fs::path& src, const fs::path& build, const char* target,
               bool iso_c11) {
    std::vector<std::string> cfg{SUYU_SMOKE_CMAKE, "-S", src.string(), "-B",
                                 build.string(), "-DCMAKE_BUILD_TYPE=Release"};
    const std::string gen = SUYU_SMOKE_GENERATOR;
    if (!gen.empty()) {
        cfg.push_back("-G");
        cfg.push_back(gen);
    }
    const std::string plat = SUYU_SMOKE_GENERATOR_PLATFORM;
    if (!plat.empty()) {
        cfg.push_back("-A");
        cfg.push_back(plat);
    }
    if (iso_c11) {
        // Probe sources are ISO C11. The generated runtime uses POSIX
        // nanosleep, so it is compiled with the host compiler defaults.
        cfg.emplace_back("-DCMAKE_C_STANDARD=11");
        cfg.emplace_back("-DCMAKE_C_EXTENSIONS=OFF");
    }
    const std::string cc = SUYU_SMOKE_C_COMPILER;
    // The Visual Studio generator selects cl.exe itself; passing a
    // CMAKE_C_COMPILER path is unnecessary and can confuse the cache.
    if (!cc.empty() && gen.rfind("Visual Studio", 0) != 0) {
        cfg.push_back(std::string("-DCMAKE_C_COMPILER=") + cc);
    }
    if (RunArgs(cfg) != 0) {
        fail("cmake configure " + src.string());
        return 1;
    }
    const std::vector<std::string> bld{SUYU_SMOKE_CMAKE, "--build", build.string(),
                                       "--config", "Release", "--target", target};
    if (RunArgs(bld) != 0) {
        fail("cmake build " + std::string(target) + " in " + build.string());
        return 1;
    }
    return 0;
}

void TestEmitProjectCompile(const fs::path& root) {
    const fs::path out = root / "emit_project";
    fs::create_directories(out);

    u32 text[4] = {kMovzX0_5, kMovzX1_7, kAddX2X0X1, kSvc0};
    suyu::recomp::EmitProject("smoke", reinterpret_cast<const suyu::recomp::u8*>(text),
                              sizeof(text), 0x1000, out.string(), true);

    const fs::path runtime = out / "recomp_runtime.c";
    if (!fs::exists(runtime)) {
        fail("EmitProject did not write recomp_runtime.c");
        return;
    }
    if (!AesHelpersAtFileScope(ReadFile(runtime))) {
        return;
    }

    if (CmakeBuild(out, out / "build", "recompiled", false) == 0) {
        pass("EmitProject generated project compiled");
    }
}

std::string TranslateInsn(u32 insn, u64 pc) {
    std::string body;
    suyu::recomp::Translate(insn, pc, body);
    return body;
}

void TestTranslatedShape() {
    const std::string ret5 = TranslateInsn(kRetX5, 0x1000);
    if (ret5.find("c->x[5]") == std::string::npos) {
        fail("RET X5 does not read X5: " + ret5);
    } else if (ret5.find("c->x[30]") != std::string::npos) {
        fail("RET X5 still mentions X30: " + ret5);
    } else {
        pass("RET X5 translated C reads X5");
    }

    const std::string blr = TranslateInsn(kBlrX30, 0x1000);
    const auto read = blr.find("c->x[30]");
    const auto write = blr.find("c->x[30]=");
    if (read == std::string::npos || write == std::string::npos) {
        fail("BLR X30 missing X30 read or link write: " + blr);
    } else if (read >= write) {
        fail("BLR X30 writes X30 before reading the target: " + blr);
    } else {
        pass("BLR X30 translated C reads the target before writing LR");
    }
}

void TestBranchProbes(const fs::path& root) {
    const std::string ret5 = TranslateInsn(kRetX5, 0x1000);
    const std::string ret30 = TranslateInsn(kRetX30, 0x1000);
    const std::string blr = TranslateInsn(kBlrX30, 0x1000);

    std::ostringstream src;
    src << "#include <stdint.h>\n#include <stdio.h>\n"
           "typedef struct { uint64_t x[32]; uint64_t pc; } GuestContext;\n"
           "uint64_t g_module_base = 0;\n"
           "static void ret_x5(GuestContext* c) {\n"
        << ret5
        << "}\nstatic void ret_x30(GuestContext* c) {\n"
        << ret30
        << "}\nstatic void blr_x30(GuestContext* c) {\n"
        << blr
        << "}\nint main(void) {\n"
           "  int fail = 0;\n"
           "  GuestContext c;\n"
           "  int i;\n"
           "  for (i = 0; i < 32; i++) c.x[i] = 0;\n"
           "  c.x[5] = 0x9000; c.x[30] = 0x8000; c.pc = 0x1000;\n"
           "  ret_x5(&c);\n"
           "  printf(\"RET X5: pc=%llx expected=9000\\n\", (unsigned long long)c.pc);\n"
           "  if (c.pc != 0x9000) fail = 1;\n"
           "  for (i = 0; i < 32; i++) c.x[i] = 0;\n"
           "  c.x[30] = 0x8000; c.pc = 0x1000;\n"
           "  ret_x30(&c);\n"
           "  printf(\"RET X30: pc=%llx expected=8000\\n\", (unsigned long long)c.pc);\n"
           "  if (c.pc != 0x8000) fail = 1;\n"
           "  for (i = 0; i < 32; i++) c.x[i] = 0;\n"
           "  c.x[30] = 0x8000; c.pc = 0x1000;\n"
           "  blr_x30(&c);\n"
           "  printf(\"BLR X30: pc=%llx expected=8000 lr=%llx expected=1004\\n\",\n"
           "         (unsigned long long)c.pc, (unsigned long long)c.x[30]);\n"
           "  if (c.pc != 0x8000 || c.x[30] != 0x1004) fail = 1;\n"
           "  return fail;\n"
           "}\n";

    const fs::path probe_src = root / "branch_probe";
    fs::create_directories(probe_src);
    if (!WriteFile(probe_src / "probe.c", src.str())) {
        return;
    }
    if (!WriteFile(probe_src / "CMakeLists.txt",
                   "cmake_minimum_required(VERSION 3.13)\n"
                   "project(suyu_branch_probe C)\n"
                   "set(CMAKE_C_STANDARD 11)\n"
                   "set(CMAKE_C_EXTENSIONS OFF)\n"
                   "add_executable(branch_probe probe.c)\n")) {
        return;
    }

    const fs::path probe_build = probe_src / "build";
    if (CmakeBuild(probe_src, probe_build, "branch_probe", true) != 0) {
        return;
    }

    fs::path exe = probe_build / "branch_probe";
#ifdef _WIN32
    if (!fs::exists(exe)) {
        exe = probe_build / "Release" / "branch_probe.exe";
    }
    if (!fs::exists(exe)) {
        exe = probe_build / "Debug" / "branch_probe.exe";
    }
#else
    if (!fs::exists(exe)) {
        exe = probe_build / "Release" / "branch_probe";
    }
#endif
    if (!fs::exists(exe)) {
        fail("branch_probe executable not found under " + probe_build.string());
        return;
    }
    if (RunArgs({exe.string()}) != 0) {
        fail("branch_probe execution");
        return;
    }
    pass("RET X5 / RET X30 / BLR X30 executed");
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root =
        fs::temp_directory_path() / ("suyu-exporter-smoke-" + std::to_string(stamp));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    std::cout << "exporter smoke workdir: " << root << std::endl;
    TestTranslatedShape();
    TestEmitProjectCompile(root);
    TestBranchProbes(root);

    if (const char* ev = std::getenv("SUYU_SMOKE_EVIDENCE_DIR")) {
        const fs::path dest(ev);
        fs::create_directories(dest);
        const fs::path runtime = root / "emit_project" / "recomp_runtime.c";
        const fs::path probe = root / "branch_probe" / "probe.c";
        if (fs::exists(runtime)) {
            fs::copy_file(runtime, dest / "recomp_runtime.c",
                          fs::copy_options::overwrite_existing);
        }
        if (fs::exists(probe)) {
            fs::copy_file(probe, dest / "branch_probe.c",
                          fs::copy_options::overwrite_existing);
        }
        std::cout << "copied evidence to " << dest << std::endl;
    }

    if (g_fails == 0) {
        fs::remove_all(root, ec);
        std::cout << "exporter_smoke: all checks passed" << std::endl;
        return 0;
    }
    std::cerr << "exporter_smoke: " << g_fails << " failure(s); keeping " << root << "\n";
    return 1;
}
