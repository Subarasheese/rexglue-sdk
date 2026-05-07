/**
 * @file        tests/unit/rexglue/upgrade_detect_test.cpp
 * @brief       Tests for upgrade detection plan builder
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include "rexglue/commands/upgrade_detect.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

struct TempProject {
  fs::path root;
  TempProject() : root(fs::temp_directory_path() / "upgrade_detect_test") {
    fs::remove_all(root);
    fs::create_directories(root / "generated");
  }
  ~TempProject() { fs::remove_all(root); }

  void writeRexglueCmake(const std::string& content) const {
    std::ofstream f(root / "generated" / "rexglue.cmake");
    f << content;
  }

  void writeFile(const fs::path& rel, const std::string& content) const {
    fs::create_directories((root / rel).parent_path());
    std::ofstream f(root / rel);
    f << content;
  }

  std::string readFile(const fs::path& rel) const {
    std::ifstream f(root / rel);
    std::string out;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return out;
  }
};

}  // namespace

TEST_CASE("UpgradeDetect: empty plan when rexglue.cmake matches template",
          "[rexglue][upgrade_detect]") {
  TempProject tp;
  rexglue::cli::UpgradeDetect detect;
  std::string rendered = detect.RenderRexglueCmake("mygame", "0.8.0", "generated/default");
  tp.writeRexglueCmake(rendered);

  auto plan = detect.Plan(tp.root, "mygame", "0.8.0", "generated/default");
  CHECK(plan.empty());
}

TEST_CASE("UpgradeDetect: rexglue.cmake entry is silent (lives inside generated/)",
          "[rexglue][upgrade_detect]") {
  TempProject tp;
  tp.writeRexglueCmake("# obviously stale content\n");

  rexglue::cli::UpgradeDetect detect;
  auto plan = detect.Plan(tp.root, "mygame", "0.8.0", "generated/default");
  REQUIRE(plan.size() == 1u);
  CHECK(plan[0].path.filename() == "rexglue.cmake");
  CHECK(plan[0].silent);
  CHECK(plan[0].action == rexglue::cli::OverwriteAction::Write);
  CHECK(plan[0].rendered_content.find("rex::runtime") != std::string::npos);
  CHECK(plan[0].rendered_content.find("generated/default/sources.cmake") != std::string::npos);
}

TEST_CASE("UpgradeDetect: rexglue.cmake entry is silent when missing",
          "[rexglue][upgrade_detect]") {
  TempProject tp;  // generated/rexglue.cmake never written

  rexglue::cli::UpgradeDetect detect;
  auto plan = detect.Plan(tp.root, "mygame", "0.8.0", "generated/default");
  REQUIRE(plan.size() == 1u);
  CHECK(plan[0].silent);
  CHECK(plan[0].path == tp.root / "generated" / "rexglue.cmake");
}

TEST_CASE("UpgradeDetect: ScanCmakeReferences rewrites legacy config refs",
          "[rexglue][upgrade_detect]") {
  TempProject tp;
  tp.writeFile("CMakeLists.txt",
               "add_custom_target(mygame_codegen\n"
               "    COMMAND rexglue codegen ${CMAKE_CURRENT_SOURCE_DIR}/mygame_config.toml\n"
               ")\n");
  tp.writeFile("cmake/extra.cmake", "# nothing to do here\n");

  rexglue::cli::UpgradeDetect detect;
  auto entries = detect.ScanCmakeReferences(tp.root, "mygame_config.toml", "mygame_manifest.toml");
  REQUIRE(entries.size() == 1u);
  CHECK(entries[0].path == tp.root / "CMakeLists.txt");
  CHECK_FALSE(entries[0].silent);
  CHECK(entries[0].rendered_content.find("mygame_manifest.toml") != std::string::npos);
  CHECK(entries[0].rendered_content.find("mygame_config.toml") == std::string::npos);
}

TEST_CASE("UpgradeDetect: ScanCmakeReferences leaves embedded substrings alone",
          "[rexglue][upgrade_detect]") {
  TempProject tp;
  // foo_config.toml.bak and a comment containing the legacy name must not
  // be rewritten; only standalone path tokens should match.
  tp.writeFile("CMakeLists.txt",
               "# legacy file: mygame_config.toml.bak retained for reference\n"
               "add_custom_target(mygame_codegen\n"
               "    COMMAND rexglue codegen mygame_config.toml\n"
               ")\n");
  rexglue::cli::UpgradeDetect detect;
  auto entries = detect.ScanCmakeReferences(tp.root, "mygame_config.toml", "mygame_manifest.toml");
  REQUIRE(entries.size() == 1u);
  CHECK(entries[0].rendered_content.find("mygame_config.toml.bak") != std::string::npos);
  size_t cmd_pos = entries[0].rendered_content.find("rexglue codegen mygame_manifest.toml");
  CHECK(cmd_pos != std::string::npos);
}

TEST_CASE("UpgradeDetect: ScanCmakeReferences skips files inside generated/",
          "[rexglue][upgrade_detect]") {
  TempProject tp;
  tp.writeFile("generated/rexglue.cmake",
               "add_custom_target(mygame_codegen\n"
               "    COMMAND rexglue codegen mygame_config.toml\n"
               ")\n");

  rexglue::cli::UpgradeDetect detect;
  auto entries = detect.ScanCmakeReferences(tp.root, "mygame_config.toml", "mygame_manifest.toml");
  CHECK(entries.empty());
}

TEST_CASE("UpgradeDetect: ScanCmakeReferences ignores irrelevant files",
          "[rexglue][upgrade_detect]") {
  TempProject tp;
  tp.writeFile("README.md", "mygame_config.toml is the legacy config\n");
  tp.writeFile("src/main.cpp", "// references mygame_config.toml in a comment\n");

  rexglue::cli::UpgradeDetect detect;
  auto entries = detect.ScanCmakeReferences(tp.root, "mygame_config.toml", "mygame_manifest.toml");
  CHECK(entries.empty());
}

TEST_CASE("UpgradeDetect: ScanSourceIncludeRewrites renames _config.h to _init.h",
          "[rexglue][upgrade_detect]") {
  TempProject tp;
  tp.writeFile("src/main.cpp",
               "#include \"generated/mygame_config.h\"\n"
               "int main() { return 0; }\n");

  rexglue::cli::UpgradeDetect detect;
  auto entries = detect.ScanSourceIncludeRewrites(tp.root, "mygame");
  REQUIRE(entries.size() == 1u);
  CHECK(entries[0].path == tp.root / "src" / "main.cpp");
  CHECK_FALSE(entries[0].silent);
  CHECK(entries[0].action == rexglue::cli::OverwriteAction::Write);
  CHECK(entries[0].rendered_content.find("generated/mygame_init.h") != std::string::npos);
  CHECK(entries[0].rendered_content.find("mygame_config.h") == std::string::npos);
  CHECK(entries[0].rendered_content.find("int main()") != std::string::npos);
}

TEST_CASE("UpgradeDetect: ScanSourceIncludeRewrites drops duplicate when _init.h already present",
          "[rexglue][upgrade_detect]") {
  TempProject tp;
  tp.writeFile("src/main.cpp",
               "#include \"generated/mygame_config.h\"\n"
               "#include \"generated/mygame_init.h\"\n"
               "int main() { return 0; }\n");

  rexglue::cli::UpgradeDetect detect;
  auto entries = detect.ScanSourceIncludeRewrites(tp.root, "mygame");
  REQUIRE(entries.size() == 1u);
  // The stale _config.h line should be removed, leaving just the _init.h
  // include and the rest of the file intact.
  CHECK(entries[0].rendered_content.find("mygame_config.h") == std::string::npos);
  size_t init_count = 0;
  for (size_t pos = 0;
       (pos = entries[0].rendered_content.find("mygame_init.h", pos)) != std::string::npos;
       pos += 1) {
    ++init_count;
  }
  CHECK(init_count == 1u);
  CHECK(entries[0].rendered_content.find("int main()") != std::string::npos);
}

TEST_CASE("UpgradeDetect: ScanSourceIncludeRewrites no-op when no _config.h reference",
          "[rexglue][upgrade_detect]") {
  TempProject tp;
  tp.writeFile("src/main.cpp",
               "#include \"generated/mygame_init.h\"\n"
               "int main() { return 0; }\n");

  rexglue::cli::UpgradeDetect detect;
  auto entries = detect.ScanSourceIncludeRewrites(tp.root, "mygame");
  CHECK(entries.empty());
}

TEST_CASE("UpgradeDetect: ScanSourceIncludeRewrites skips files inside generated/",
          "[rexglue][upgrade_detect]") {
  TempProject tp;
  tp.writeFile("generated/mygame_register.cpp", "#include \"mygame_config.h\"\n");

  rexglue::cli::UpgradeDetect detect;
  auto entries = detect.ScanSourceIncludeRewrites(tp.root, "mygame");
  CHECK(entries.empty());
}

TEST_CASE("UpgradeDetect: ScanSourceIncludeRewrites only matches the project's own header",
          "[rexglue][upgrade_detect]") {
  TempProject tp;
  // A different project's _config.h must be left alone -- only the configured
  // project's basename triggers the rename.
  tp.writeFile("src/main.cpp", "#include \"otherproj_config.h\"\n");

  rexglue::cli::UpgradeDetect detect;
  auto entries = detect.ScanSourceIncludeRewrites(tp.root, "mygame");
  CHECK(entries.empty());
}

TEST_CASE("UpgradeDetect: ScanSourceIncludeRewrites is case-insensitive on basename",
          "[rexglue][upgrade_detect]") {
  TempProject tp;
  tp.writeFile("src/main.cpp", "#include \"MyGame_Config.H\"\n");

  rexglue::cli::UpgradeDetect detect;
  auto entries = detect.ScanSourceIncludeRewrites(tp.root, "mygame");
  REQUIRE(entries.size() == 1u);
  CHECK(entries[0].rendered_content.find("mygame_init.h") != std::string::npos);
}
