/**
 * @file        rexglue/commands/init_command.cpp
 * @brief       Project initialization command implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "init_command.h"
#include "template_utils.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <rex/codegen/manifest.h>
#include <rex/codegen/template_registry.h>
#include <rex/logging.h>
#include <rex/result.h>
#include <rex/version.h>

#include <fmt/chrono.h>
#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

namespace fs = std::filesystem;

namespace rexglue::cli {

using rex::Err;
using rex::Error;
using rex::ErrorCategory;
using rex::Ok;

namespace {

/**
 * Render `target` relative to `base` using forward slashes. If `target` is
 * not inside `base`, returns the absolute path so the manifest is still
 * well-formed.
 */
std::string RelativeToBase(const fs::path& target, const fs::path& base) {
  std::error_code ec;
  fs::path rel = fs::relative(target, base, ec);
  if (ec || rel.empty()) {
    return target.generic_string();
  }
  return rel.generic_string();
}

std::string LowercaseAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

/**
 * Canonical form for any path written into the manifest: forward slashes
 * (TOML basic-string safe) and lowercase ASCII (matches guest_path
 * normalization, so all path-shaped fields share a single transform).
 */
std::string ManifestPath(const fs::path& target, const fs::path& base) {
  return LowercaseAscii(RelativeToBase(target, base));
}

std::string IsoUtcStamp() {
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  return fmt::format("{:%Y-%m-%d %H:%M:%S} UTC", fmt::gmtime(t));
}

}  // namespace

Result<void> InitProject(const InitOptions& opts, const CliContext& ctx) {
  (void)ctx;

  if (opts.project_name.empty()) {
    return Err<void>(ErrorCategory::Config, "--project_name is required");
  }
  if (opts.xex_path.empty()) {
    return Err<void>(ErrorCategory::Config, "--xex_path is required (path to entrypoint XEX)");
  }

  std::string validation_error;
  if (!validate_app_name(opts.project_name, validation_error)) {
    return Err<void>(ErrorCategory::Config, validation_error);
  }
  auto names = parse_app_name(opts.project_name);

  std::error_code ec;
  fs::path projectRoot =
      opts.project_root.empty() ? fs::current_path(ec) : fs::absolute(opts.project_root, ec);
  if (ec) {
    return Err<void>(ErrorCategory::IO, "Failed to resolve project root: " + ec.message());
  }
  projectRoot = fs::weakly_canonical(projectRoot, ec);
  if (ec) {
    ec.clear();
    projectRoot = fs::absolute(projectRoot);
  }

  fs::path xexAbs = fs::absolute(opts.xex_path, ec);
  if (ec) {
    return Err<void>(ErrorCategory::IO,
                     "Failed to resolve --xex_path '" + opts.xex_path + "': " + ec.message());
  }
  xexAbs = fs::weakly_canonical(xexAbs, ec);
  if (ec) {
    ec.clear();
    xexAbs = fs::absolute(opts.xex_path);
  }
  if (!fs::exists(xexAbs)) {
    return Err<void>(ErrorCategory::IO, "Entrypoint XEX not found: " + xexAbs.string() +
                                            " (from --xex_path " + opts.xex_path + ")");
  }
  if (!fs::is_regular_file(xexAbs)) {
    return Err<void>(ErrorCategory::IO, "--xex_path is not a regular file: " + xexAbs.string());
  }

  std::string xexStem = xexAbs.stem().string();
  if (xexStem.empty()) {
    return Err<void>(ErrorCategory::Config, "--xex_path has no filename: " + opts.xex_path);
  }

  fs::path gameRootAbs;
  if (opts.game_root.empty()) {
    gameRootAbs = xexAbs.parent_path();
  } else {
    gameRootAbs = fs::absolute(opts.game_root, ec);
    if (ec) {
      return Err<void>(ErrorCategory::IO,
                       "Failed to resolve --game_root '" + opts.game_root + "': " + ec.message());
    }
    gameRootAbs = fs::weakly_canonical(gameRootAbs, ec);
    if (ec) {
      ec.clear();
      gameRootAbs = fs::absolute(opts.game_root);
    }
  }
  if (!fs::exists(gameRootAbs) || !fs::is_directory(gameRootAbs)) {
    return Err<void>(ErrorCategory::IO, "--game_root is not a directory: " + gameRootAbs.string());
  }

  // The entrypoint XEX must be inside game_root so its guest_path lines up
  // with the runtime's content root.
  {
    fs::path xexRelToGame = fs::relative(xexAbs, gameRootAbs, ec);
    if (ec || xexRelToGame.empty() || xexRelToGame.native()[0] == '.') {
      return Err<void>(ErrorCategory::Config, "--xex_path (" + xexAbs.string() +
                                                  ") is not inside --game_root (" +
                                                  gameRootAbs.string() + ")");
    }
  }

  std::string outDir = LowercaseAscii("generated/" + xexStem);
  std::string xexRelManifest = ManifestPath(xexAbs, projectRoot);
  std::string gameRootRelManifest = ManifestPath(gameRootAbs, projectRoot);

  // Discover DLL modules under game_root when --dll was passed. Each entry
  // becomes a [[modules]] table in the manifest with file_path relative to
  // the manifest dir and guest_path canonicalized from its location under
  // game_root.
  nlohmann::json modulesJson = nlohmann::json::array();
  if (opts.scan_dlls) {
    std::vector<fs::path> dllPaths;
    fs::recursive_directory_iterator it(gameRootAbs, fs::directory_options::skip_permission_denied,
                                        ec);
    fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file())
        continue;
      auto ext = it->path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (ext != ".dll")
        continue;
      dllPaths.push_back(it->path());
    }
    std::sort(dllPaths.begin(), dllPaths.end());

    for (const auto& dllAbs : dllPaths) {
      fs::path relUnderGame = fs::relative(dllAbs, gameRootAbs, ec);
      if (ec || relUnderGame.empty()) {
        ec.clear();
        continue;
      }
      std::string guestPath = rex::codegen::CanonicalizeModuleGuestPath(
          relUnderGame.generic_string(), names.snake_case);
      std::string filePath = ManifestPath(dllAbs, projectRoot);
      std::string moduleStem = LowercaseAscii(dllAbs.stem().string());
      std::replace(moduleStem.begin(), moduleStem.end(), '.', '_');
      std::replace(moduleStem.begin(), moduleStem.end(), ' ', '_');
      modulesJson.push_back({
          {"guest_path", guestPath},
          {"file_path", filePath},
          {"out_directory_path", "generated/" + moduleStem},
      });
    }
  }

  rex::codegen::TemplateRegistry registry;
  if (!opts.template_dir.empty())
    registry.loadOverrides(opts.template_dir);

  nlohmann::json data = {
      {"names", names_to_json(names)},
      {"sdk_version", REXGLUE_VERSION_NUMERIC},
      {"sdk_version_full", REXGLUE_VERSION_STRING},
      {"generated_on", IsoUtcStamp()},
      {"include_stamp", true},
      {"xex_path", xexRelManifest},
      {"game_root", gameRootRelManifest},
      {"out_directory_path", outDir},
      {"entrypoint_out_dir", outDir},
      {"modules", modulesJson},
  };
  std::string jsonStr = data.dump();

  fs::path root = projectRoot;

  REXLOG_INFO("Initializing project '{}' at: {}", names.snake_case, root.string());
  REXLOG_INFO("  Entrypoint XEX: {}", xexRelManifest);
  REXLOG_INFO("  Game root:      {}", gameRootRelManifest);
  if (opts.scan_dlls) {
    REXLOG_INFO("  DLL modules:    {}", modulesJson.size());
    for (const auto& m : modulesJson) {
      REXLOG_INFO("    - {} -> {}", m["file_path"].get<std::string>(),
                  m["guest_path"].get<std::string>());
    }
  }

  // Refuse to overwrite an existing codegen manifest unless --force is set.
  // Other files in the directory are left alone; only a sibling TOML that
  // parses as a manifest counts as a conflict.
  if (fs::exists(root)) {
    if (!fs::is_directory(root)) {
      return Err<void>(ErrorCategory::IO, "Path exists but is not a directory: " + root.string());
    }

    if (!opts.force) {
      for (const auto& entry : fs::directory_iterator(root)) {
        if (!entry.is_regular_file())
          continue;
        if (entry.path().extension() != ".toml")
          continue;
        if (rex::codegen::ManifestConfig::IsManifest(entry.path())) {
          return Err<void>(ErrorCategory::IO,
                           "Existing codegen manifest found. Use --force to overwrite: " +
                               entry.path().string());
        }
      }
    }
  }

  // Create directory structure
  REXLOG_INFO("Creating directory structure...");

  fs::create_directories(root, ec);
  if (ec) {
    return Err<void>(ErrorCategory::IO, "Failed to create root directory: " + ec.message());
  }

  fs::create_directories(root / "src", ec);
  if (ec) {
    return Err<void>(ErrorCategory::IO, "Failed to create src directory: " + ec.message());
  }

  fs::create_directories(root / "generated", ec);
  if (ec) {
    return Err<void>(ErrorCategory::IO, "Failed to create generated directory: " + ec.message());
  }

  // Generate files
  REXLOG_INFO("Generating project files...");

  if (!write_file(root / "CMakeLists.txt", registry.render("init/cmakelists", jsonStr))) {
    return Err<void>(ErrorCategory::IO, "Failed to write CMakeLists.txt");
  }
  REXLOG_DEBUG("  Created CMakeLists.txt");

  // generated/rexglue.cmake (SDK-managed)
  if (!write_file(root / "generated" / "rexglue.cmake",
                  registry.render("init/rexglue_cmake", jsonStr))) {
    return Err<void>(ErrorCategory::IO, "Failed to write generated/rexglue.cmake");
  }
  REXLOG_DEBUG("  Created generated/rexglue.cmake");

  if (!write_file(root / "src" / "main.cpp", registry.render("init/main_cpp", jsonStr))) {
    return Err<void>(ErrorCategory::IO, "Failed to write main.cpp");
  }
  REXLOG_DEBUG("  Created src/main.cpp");

  // src/{name}_app.h (user-owned)
  std::string app_header_filename = names.snake_case + "_app.h";
  if (!write_file(root / "src" / app_header_filename,
                  registry.render("init/app_header", jsonStr))) {
    return Err<void>(ErrorCategory::IO, "Failed to write src/" + app_header_filename);
  }
  REXLOG_DEBUG("  Created src/{}", app_header_filename);

  std::string manifest_filename = names.snake_case + "_manifest.toml";
  if (!write_file(root / manifest_filename, registry.render("init/manifest_toml", jsonStr))) {
    return Err<void>(ErrorCategory::IO, "Failed to write " + manifest_filename);
  }
  REXLOG_DEBUG("  Created {}", manifest_filename);

  if (!write_file(root / "CMakePresets.json", registry.render("init/cmake_presets", jsonStr))) {
    return Err<void>(ErrorCategory::IO, "Failed to write CMakePresets.json");
  }
  REXLOG_DEBUG("  Created CMakePresets.json");

  // Print success message with next steps
  REXLOG_INFO("Project '{}' initialized in '{}' successfully!", names.snake_case, root.string());

  return Ok();
}

Result<void> InitModule(const InitModuleOptions& opts, const CliContext& ctx) {
  namespace fs = std::filesystem;
  (void)ctx;

  fs::path root = fs::absolute(opts.app_root);
  std::error_code dir_ec;
  fs::path manifestPath;
  for (const auto& entry : fs::directory_iterator(root, dir_ec)) {
    if (entry.path().extension() == ".toml" &&
        rex::codegen::ManifestConfig::IsManifest(entry.path())) {
      manifestPath = entry.path();
      break;
    }
  }
  if (dir_ec) {
    return Err<void>(ErrorCategory::IO, fmt::format("Cannot read project root '{}': {}",
                                                    root.string(), dir_ec.message()));
  }
  if (manifestPath.empty()) {
    return Err<void>(rex::ErrorCategory::Config,
                     "No manifest found in project root. Run 'rexglue init' first.");
  }

  auto manifest = rex::codegen::ManifestConfig::Load(manifestPath);
  if (!manifest) {
    return Err<void>(rex::ErrorCategory::Config, "Failed to load manifest");
  }

  fs::path xexFile(opts.xex_path);
  std::string moduleName = xexFile.stem().string();
  std::replace(moduleName.begin(), moduleName.end(), '.', '_');
  std::replace(moduleName.begin(), moduleName.end(), ' ', '_');
  std::transform(moduleName.begin(), moduleName.end(), moduleName.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  fs::path absoluteXexPath = fs::weakly_canonical(fs::absolute(opts.xex_path));
  if (!fs::exists(absoluteXexPath)) {
    return Err<void>(rex::ErrorCategory::IO,
                     fmt::format("XEX file not found: {} (resolved from {})",
                                 absoluteXexPath.string(), opts.xex_path));
  }

  fs::path relXexPath = fs::relative(absoluteXexPath, root);
  std::string xexPath = relXexPath.generic_string();

  std::string guestPath =
      rex::codegen::CanonicalizeModuleGuestPath(opts.guest_path, manifest->projectName);

  // Parse-only check for duplicates and to surface syntax errors.
  toml::table manifestTbl;
  try {
    manifestTbl = toml::parse_file(manifestPath.string());
  } catch (const toml::parse_error& err) {
    return Err<void>(rex::ErrorCategory::Config,
                     fmt::format("Manifest parse error: {}", err.what()));
  }
  if (auto* modulesArr = manifestTbl["modules"].as_array()) {
    for (const auto& mod : *modulesArr) {
      auto* modTbl = mod.as_table();
      if (!modTbl)
        continue;
      auto existing_file = (*modTbl)["file_path"].value_or<std::string>("");
      auto existing_guest = (*modTbl)["guest_path"].value_or<std::string>("");
      if (existing_file == xexPath || existing_guest == guestPath) {
        REXLOG_INFO("Manifest already lists this module; nothing to do.");
        return Ok();
      }
    }
  }

  // Append to the file textually so user comments and formatting survive.
  std::ifstream in(manifestPath);
  if (!in) {
    return Err<void>(rex::ErrorCategory::IO,
                     "Failed to open manifest for reading: " + manifestPath.string());
  }
  std::stringstream ss;
  ss << in.rdbuf();
  std::string content = ss.str();
  in.close();

  if (!content.empty() && content.back() != '\n') {
    content.push_back('\n');
  }

  std::string entry;
  entry += "\n[[modules]]\n";
  entry += fmt::format("guest_path = \"{}\"\n", guestPath);
  entry += fmt::format("file_path = \"{}\"\n", xexPath);
  entry += fmt::format("out_directory_path = \"generated/{}\"\n", moduleName);
  entry += "includes = []\n";
  content += entry;

  auto tmpPath = manifestPath;
  tmpPath += ".tmp";
  {
    std::ofstream out(tmpPath, std::ios::binary);
    if (!out) {
      return Err<void>(rex::ErrorCategory::IO,
                       "Failed to open manifest tmp for writing: " + tmpPath.string());
    }
    out << content;
    if (!out.good()) {
      std::error_code ignore;
      fs::remove(tmpPath, ignore);
      return Err<void>(rex::ErrorCategory::IO,
                       "Failed while writing manifest tmp: " + tmpPath.string());
    }
  }
  std::error_code ec;
  fs::rename(tmpPath, manifestPath, ec);
  if (ec) {
    std::error_code ignore;
    fs::remove(tmpPath, ignore);
    return Err<void>(rex::ErrorCategory::IO,
                     "Failed to rename manifest tmp into place: " + ec.message());
  }

  REXLOG_INFO("Module '{}' added to manifest", moduleName);
  REXLOG_INFO("  file_path:  {}", xexPath);
  REXLOG_INFO("  guest_path: {}", guestPath);

  return Ok();
}

}  // namespace rexglue::cli
