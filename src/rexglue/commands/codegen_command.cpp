/**
 * @file        rexglue/commands/codegen_command.cpp
 * @brief       Code generation command implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "codegen_command.h"
#include "include_scan.h"
#include "template_utils.h"
#include "upgrade_detect.h"
#include "upgrade_prompt.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <toml++/toml.hpp>

#include <rex/codegen/manifest.h>
#include <rex/codegen/project_recompiler.h>
#include <rex/logging.h>
#include <rex/version.h>

namespace rexglue::cli {

namespace {

namespace fs = std::filesystem;

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

struct AbsorbedManifest {
  fs::path manifest_path;
  fs::path legacy_path;
  std::string project_name;
  std::string out_directory_path;
  std::string manifest_content;         ///< New <name>_manifest.toml content
  std::string stripped_legacy_content;  ///< Legacy file overwritten with sub-tables only
};

/**
 * Returns the table name when `line` is a non-array section header like
 * `[name]`, otherwise nullopt.
 */
std::optional<std::string> ParseSectionHeader(std::string_view line) {
  auto first = line.find_first_not_of(" \t");
  if (first == std::string_view::npos)
    return std::nullopt;
  if (line[first] != '[' || (first + 1 < line.size() && line[first + 1] == '['))
    return std::nullopt;
  auto end = line.find(']', first + 1);
  if (end == std::string_view::npos)
    return std::nullopt;
  return std::string(line.substr(first + 1, end - first - 1));
}

bool LineSetsKey(std::string_view line, std::string_view key) {
  auto first = line.find_first_not_of(" \t");
  if (first == std::string_view::npos)
    return false;
  if (line.compare(first, key.size(), key) != 0)
    return false;
  auto after = first + key.size();
  while (after < line.size() && (line[after] == ' ' || line[after] == '\t'))
    ++after;
  return after < line.size() && line[after] == '=';
}

/**
 * Returns the index of the line that closes the value started on `start_line`.
 * For single-line scalars that's the same line; for multi-line array literals
 * it walks forward until brackets balance.
 */
size_t FindAssignmentEndLine(const std::vector<std::string>& lines, size_t start_line) {
  int depth = 0;
  bool seen_open = false;
  for (size_t j = start_line; j < lines.size(); ++j) {
    bool in_string = false;
    char quote = 0;
    for (size_t k = 0; k < lines[j].size(); ++k) {
      char c = lines[j][k];
      if (in_string) {
        if (c == '\\' && k + 1 < lines[j].size()) {
          ++k;
          continue;
        }
        if (c == quote)
          in_string = false;
        continue;
      }
      if (c == '#')
        break;
      if (c == '"' || c == '\'') {
        in_string = true;
        quote = c;
        continue;
      }
      if (c == '[') {
        ++depth;
        seen_open = true;
      } else if (c == ']') {
        --depth;
      }
    }
    if (!seen_open)
      return j;  // single-line scalar
    if (depth <= 0)
      return j;
  }
  return lines.size() - 1;
}

/**
 * TOML basic string literal with conservative escaping.
 */
std::string RenderTomlString(std::string_view value) {
  std::string out = "\"";
  for (char c : value) {
    switch (c) {
      case '\\':
        out.append("\\\\");
        break;
      case '"':
        out.append("\\\"");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

std::string RenderStringLine(std::string_view key, std::string_view value) {
  std::string out;
  out.append(key);
  out.append(" = ");
  out.append(RenderTomlString(value));
  return out;
}

/**
 * Read the legacy single-binary config and produce a thin manifest plus a
 * stripped-down version of the legacy file. The manifest holds project
 * metadata + an [entrypoint] table that points the legacy file via includes;
 * the legacy file keeps its codegen flags / [functions] / [rexcrt] / comments
 * but loses core attrs that are now in the manifest. Returns nullopt when the
 * legacy file lacks a usable project_name.
 */
std::optional<AbsorbedManifest> AbsorbLegacyConfig(const fs::path& legacy_path) {
  toml::table tbl;
  try {
    tbl = toml::parse_file(legacy_path.string());
  } catch (const toml::parse_error& err) {
    REXLOG_ERROR("Failed to parse legacy config {}: {}", legacy_path.string(), err.what());
    return std::nullopt;
  }

  auto project_name = tbl["project_name"].value_or<std::string>("");
  if (project_name.empty() || project_name == "rex") {
    return std::nullopt;
  }
  auto file_path = tbl["file_path"].value_or<std::string>("");
  if (file_path.empty()) {
    REXLOG_ERROR("Legacy config {} missing required file_path", legacy_path.string());
    return std::nullopt;
  }
  auto out_directory_path = tbl["out_directory_path"].value_or<std::string>("");
  if (out_directory_path.empty()) {
    out_directory_path = "generated/" + fs::path(file_path).stem().string();
  }

  std::string legacy_content = ReadFile(legacy_path);
  if (legacy_content.empty()) {
    return std::nullopt;
  }

  std::vector<std::string> lines;
  {
    std::string buf;
    for (char c : legacy_content) {
      if (c == '\n') {
        lines.push_back(std::move(buf));
        buf.clear();
      } else if (c != '\r') {
        buf.push_back(c);
      }
    }
    if (!buf.empty())
      lines.push_back(std::move(buf));
  }

  // Find the first section header. Everything before it is top-level content
  // (codegen scalars, comments, blanks) that belongs in the manifest's
  // [entrypoint] table. From the header onward, the legacy file owns it (as a
  // data include for [functions]/[rexcrt]/etc.).
  size_t first_section_idx = lines.size();
  for (size_t i = 0; i < lines.size(); ++i) {
    if (ParseSectionHeader(lines[i])) {
      first_section_idx = i;
      break;
    }
  }

  // Drop the keys the manifest emits explicitly so we don't duplicate them.
  static constexpr std::array<std::string_view, 7> kManifestEmittedKeys = {
      "project_name",    "file_path",         "out_directory_path", "template_dir",
      "patch_file_path", "patched_file_path", "includes",
  };

  // Walk top-level assignments. Drop comments + blanks (the port carries no
  // user prose into the manifest) and skip keys the manifest emits itself.
  std::string entrypoint_body;
  size_t i = 0;
  while (i < first_section_idx) {
    auto first_nonws = lines[i].find_first_not_of(" \t");
    if (first_nonws == std::string::npos || lines[i][first_nonws] == '#') {
      ++i;
      continue;
    }
    bool emitted_by_manifest = false;
    for (auto key : kManifestEmittedKeys) {
      if (LineSetsKey(lines[i], key)) {
        emitted_by_manifest = true;
        break;
      }
    }
    size_t end = FindAssignmentEndLine(lines, i);
    if (!emitted_by_manifest) {
      for (size_t j = i; j <= end; ++j) {
        // Drop blank lines inside multi-line array literals too; strip
        // produces a tighter block.
        auto fn = lines[j].find_first_not_of(" \t");
        if (fn == std::string::npos)
          continue;
        entrypoint_body.append(lines[j]);
        entrypoint_body.push_back('\n');
      }
    }
    i = end + 1;
  }

  // Stripped legacy = section headers and below (data only). Drop comments
  // and blanks; legacy projects mostly carry old init-template boilerplate
  // that's just noise once the manifest takes over. One blank line is
  // re-inserted before each section header for readability.
  std::string stripped;
  for (size_t j = first_section_idx; j < lines.size(); ++j) {
    auto first_nonws = lines[j].find_first_not_of(" \t");
    if (first_nonws == std::string::npos)
      continue;
    if (lines[j][first_nonws] == '#')
      continue;
    if (ParseSectionHeader(lines[j]) && !stripped.empty())
      stripped.push_back('\n');
    stripped.append(lines[j]);
    stripped.push_back('\n');
  }

  // Manifest's [entrypoint].includes references the (now stripped) legacy
  // file plus any includes the legacy file used to declare.
  std::vector<std::string> entrypoint_includes;
  if (!stripped.empty())
    entrypoint_includes.push_back(legacy_path.filename().string());
  if (auto* arr = tbl["includes"].as_array()) {
    for (const auto& elem : *arr) {
      if (auto s = elem.value<std::string>()) {
        entrypoint_includes.push_back(*s);
      }
    }
  }

  std::string manifest_content;
  manifest_content += "[project]\n";
  manifest_content += RenderStringLine("name", project_name);
  manifest_content += "\n\n[entrypoint]\n";
  manifest_content += RenderStringLine("file_path", file_path);
  manifest_content += '\n';
  manifest_content += RenderStringLine("out_directory_path", out_directory_path);
  manifest_content += '\n';
  if (auto v = tbl["template_dir"].value<std::string>()) {
    manifest_content += RenderStringLine("template_dir", *v);
    manifest_content += '\n';
  }
  if (auto v = tbl["patch_file_path"].value<std::string>()) {
    manifest_content += RenderStringLine("patch_file_path", *v);
    manifest_content += '\n';
  }
  if (auto v = tbl["patched_file_path"].value<std::string>()) {
    manifest_content += RenderStringLine("patched_file_path", *v);
    manifest_content += '\n';
  }
  if (!entrypoint_includes.empty()) {
    manifest_content += "includes = [\n";
    for (const auto& inc : entrypoint_includes) {
      manifest_content += "  ";
      manifest_content += RenderTomlString(inc);
      manifest_content += ",\n";
    }
    manifest_content += "]\n";
  }
  if (!entrypoint_body.empty()) {
    manifest_content += '\n';
    manifest_content += entrypoint_body;
    manifest_content += '\n';
  }

  AbsorbedManifest result;
  result.legacy_path = legacy_path;
  result.project_name = project_name;
  result.out_directory_path = out_directory_path;
  auto names = parse_app_name(project_name);
  result.manifest_path = legacy_path.parent_path() / (names.snake_case + "_manifest.toml");
  result.manifest_content = std::move(manifest_content);
  result.stripped_legacy_content = std::move(stripped);
  return result;
}

bool ApplyEntry(const OverwriteEntry& entry) {
  std::error_code ec;
  switch (entry.action) {
    case OverwriteAction::Write: {
      fs::create_directories(entry.path.parent_path(), ec);
      if (ec) {
        REXLOG_ERROR("Failed to create directory for {}: {}", entry.path.string(), ec.message());
        return false;
      }
      std::ofstream out(entry.path);
      if (!out) {
        REXLOG_ERROR("Failed to open for write: {}", entry.path.string());
        return false;
      }
      out << entry.rendered_content;
      if (!out.good()) {
        REXLOG_ERROR("Failed to write: {}", entry.path.string());
        return false;
      }
      return true;
    }
    case OverwriteAction::Delete: {
      if (!fs::exists(entry.path))
        return true;
      if (!fs::remove(entry.path, ec) || ec) {
        REXLOG_ERROR("Failed to delete {}: {}", entry.path.string(), ec.message());
        return false;
      }
      return true;
    }
  }
  return false;
}

const char* ActionVerb(OverwriteAction action) {
  switch (action) {
    case OverwriteAction::Write:
      return "Wrote";
    case OverwriteAction::Delete:
      return "Deleted";
  }
  return "Touched";
}

Result<void> PromptConsent(const std::vector<OverwriteEntry>& plan, std::string_view from_version,
                           std::string_view to_version, bool force) {
  std::vector<OverwriteEntry> for_prompt;
  for (const auto& entry : plan) {
    if (!entry.silent)
      for_prompt.push_back(entry);
  }
  if (for_prompt.empty()) {
    return rex::Ok();
  }
  auto consent = PromptForUpgradeConsent(for_prompt, from_version, to_version, force);
  if (!consent) {
    return Err<void>(consent.error());
  }
  if (*consent == UpgradeConsent::Declined) {
    return Err<void>(rex::ErrorCategory::UserAbort, "Upgrade declined; codegen aborted.");
  }
  return rex::Ok();
}

Result<void> ApplyPlanEntries(const std::vector<OverwriteEntry>& plan) {
  for (const auto& entry : plan) {
    if (!ApplyEntry(entry)) {
      return Err<void>(rex::ErrorCategory::IO,
                       fmt::format("Failed to apply: {}", entry.path.string()));
    }
    REXLOG_INFO("{}: {}", ActionVerb(entry.action), entry.path.generic_string());
  }
  return rex::Ok();
}

void RunStaleIncludeScan(const fs::path& manifest_path,
                         const rex::codegen::ProjectRecompiler& recompiler) {
  std::unordered_set<std::string> written(recompiler.writtenFiles().begin(),
                                          recompiler.writtenFiles().end());
  std::unordered_set<std::string> removed;
  for (const auto& f : recompiler.deletedFiles()) {
    if (!written.contains(f)) {
      removed.insert(f);
    }
  }
  if (removed.empty())
    return;

  fs::path src_dir = manifest_path.parent_path() / "src";
  auto matches = ScanForStaleIncludes(src_dir, removed);
  if (matches.empty())
    return;

  REXLOG_WARN("{} source file(s) reference headers no longer emitted by codegen:", matches.size());
  for (const auto& m : matches) {
    REXLOG_WARN("  {}:{}: {}", m.file.generic_string(), m.line_number, m.raw_line);
  }
  REXLOG_WARN("These headers were emitted by older SDK versions. Update the includes by hand.");
}

Result<void> RunManifest(const fs::path& manifest_path, const CliContext& ctx) {
  auto manifest = rex::codegen::ManifestConfig::Load(manifest_path);
  if (!manifest) {
    return Err<void>(rex::ErrorCategory::Config, "Failed to load manifest");
  }
  rex::codegen::ProjectRecompiler recompiler(std::move(*manifest));
  rex::codegen::ProjectRecompilerOptions opts{
      .targets = ctx.targets,
      .force = ctx.generate_despite_errors,
      .enableExceptionHandlers = ctx.enableExceptionHandlers,
  };
  auto result = recompiler.Run(opts);
  if (!result)
    return result;

  RunStaleIncludeScan(manifest_path, recompiler);
  return rex::Ok();
}

}  // namespace

Result<std::string> DiscoverManifestInCwd() {
  std::error_code ec;
  fs::path cwd = fs::current_path(ec);
  if (ec) {
    return Err<std::string>(rex::ErrorCategory::IO,
                            fmt::format("Cannot read current directory: {}", ec.message()));
  }

  std::vector<fs::path> manifests;
  std::vector<fs::path> other_tomls;
  for (const auto& entry : fs::directory_iterator(cwd, ec)) {
    if (ec)
      break;
    if (!entry.is_regular_file())
      continue;
    if (entry.path().extension() != ".toml")
      continue;
    auto stem = entry.path().stem().string();
    if (stem.size() >= 9 && std::string_view{stem}.substr(stem.size() - 9) == "_manifest") {
      manifests.push_back(entry.path());
    } else {
      other_tomls.push_back(entry.path());
    }
  }

  if (manifests.size() == 1) {
    return rex::Ok(manifests.front().string());
  }
  if (manifests.size() > 1) {
    return Err<std::string>(
        rex::ErrorCategory::Config,
        fmt::format("Multiple *_manifest.toml files in {}; pass one explicitly", cwd.string()));
  }
  if (other_tomls.size() == 1) {
    return rex::Ok(other_tomls.front().string());
  }
  if (other_tomls.size() > 1) {
    return Err<std::string>(
        rex::ErrorCategory::Config,
        fmt::format("Multiple .toml files in {}; pass the manifest explicitly", cwd.string()));
  }
  return Err<std::string>(rex::ErrorCategory::Config,
                          fmt::format("No manifest .toml found in {}", cwd.string()));
}

Result<void> CodegenFromConfig(const std::string& config_path, const CliContext& ctx) {
  REXLOG_INFO("Generating code with config: {}", config_path);

  fs::path manifest_path;
  std::string from_version;
  std::string project_name;

  // pre_plan runs before codegen; post_plan only on codegen success so a
  // failure doesn't leave the project half-migrated.
  std::vector<OverwriteEntry> pre_plan;
  std::vector<OverwriteEntry> post_plan;
  bool fresh_absorb = false;

  const std::string current_version = REXGLUE_VERSION_NUMERIC;
  std::string entrypoint_out_dir;

  toml::table parsed_tbl;
  try {
    parsed_tbl = toml::parse_file(config_path);
  } catch (const toml::parse_error& err) {
    return Err<void>(rex::ErrorCategory::Config,
                     fmt::format("Failed to parse {}: {}", config_path, err.what()));
  }

  if (parsed_tbl.contains("project")) {
    manifest_path = config_path;
    auto manifest = rex::codegen::ManifestConfig::Load(manifest_path);
    if (!manifest) {
      return Err<void>(rex::ErrorCategory::Config, "Failed to load manifest");
    }
    project_name = manifest->projectName;
    from_version = manifest->sdkVersion.value_or("");
    entrypoint_out_dir = manifest->entrypoint.recompiler.outDirectoryPath;

    UpgradeDetect detect;
    auto drift =
        detect.Plan(manifest->manifestDir, project_name, current_version, entrypoint_out_dir);
    post_plan.insert(post_plan.end(), drift.begin(), drift.end());
    auto include_rewrites = detect.ScanSourceIncludeRewrites(manifest->manifestDir, project_name);
    post_plan.insert(post_plan.end(), include_rewrites.begin(), include_rewrites.end());
  } else {
    fs::path legacy_path = config_path;
    auto absorbed = AbsorbLegacyConfig(legacy_path);
    if (!absorbed) {
      return Err<void>(rex::ErrorCategory::Config,
                       fmt::format("Cannot absorb legacy config {}: missing required fields "
                                   "(project_name / file_path)",
                                   legacy_path.string()));
    }

    manifest_path = absorbed->manifest_path;
    project_name = absorbed->project_name;
    entrypoint_out_dir = absorbed->out_directory_path;

    if (fs::exists(manifest_path)) {
      REXLOG_WARN("Both {} and {} exist; using the manifest. Remove the legacy file when ready.",
                  manifest_path.filename().string(), legacy_path.filename().string());
      auto manifest = rex::codegen::ManifestConfig::Load(manifest_path);
      if (!manifest) {
        return Err<void>(rex::ErrorCategory::Config, "Failed to load manifest");
      }
      from_version = manifest->sdkVersion.value_or("");
      entrypoint_out_dir = manifest->entrypoint.recompiler.outDirectoryPath;
      UpgradeDetect detect;
      auto drift =
          detect.Plan(manifest->manifestDir, project_name, current_version, entrypoint_out_dir);
      post_plan.insert(post_plan.end(), drift.begin(), drift.end());
      auto include_rewrites = detect.ScanSourceIncludeRewrites(manifest->manifestDir, project_name);
      post_plan.insert(post_plan.end(), include_rewrites.begin(), include_rewrites.end());
    } else {
      fresh_absorb = true;
      pre_plan.push_back({manifest_path, std::move(absorbed->manifest_content),
                          OverwriteAction::Write, /*silent=*/false,
                          fmt::format("upgrade format to v{} standard", current_version)});
      if (!absorbed->stripped_legacy_content.empty()) {
        post_plan.push_back({legacy_path, std::move(absorbed->stripped_legacy_content),
                             OverwriteAction::Write, /*silent=*/false,
                             "strip absorbed fields (kept as include target)"});
      } else {
        post_plan.push_back({legacy_path, "", OverwriteAction::Delete, /*silent=*/false,
                             "absorbed into the new manifest"});
      }

      UpgradeDetect detect;
      auto cmake_rewrites =
          detect.ScanCmakeReferences(legacy_path.parent_path(), legacy_path.filename().string(),
                                     manifest_path.filename().string());
      post_plan.insert(post_plan.end(), cmake_rewrites.begin(), cmake_rewrites.end());

      auto drift =
          detect.Plan(legacy_path.parent_path(), project_name, current_version, entrypoint_out_dir);
      post_plan.insert(post_plan.end(), drift.begin(), drift.end());
      auto include_rewrites =
          detect.ScanSourceIncludeRewrites(legacy_path.parent_path(), project_name);
      post_plan.insert(post_plan.end(), include_rewrites.begin(), include_rewrites.end());
    }
  }

  std::vector<OverwriteEntry> consent_view;
  consent_view.insert(consent_view.end(), pre_plan.begin(), pre_plan.end());
  consent_view.insert(consent_view.end(), post_plan.begin(), post_plan.end());
  auto consent =
      PromptConsent(consent_view, from_version, current_version, ctx.skip_upgrade_consent);
  if (!consent)
    return consent;

  auto pre_applied = ApplyPlanEntries(pre_plan);
  if (!pre_applied)
    return pre_applied;

  auto run_result = RunManifest(manifest_path, ctx);
  if (!run_result) {
    if (fresh_absorb) {
      std::error_code ec;
      fs::remove(manifest_path, ec);
      REXLOG_ERROR("Codegen failed; manifest write rolled back. Legacy config is unchanged.");
    }
    return run_result;
  }

  auto post_applied = ApplyPlanEntries(post_plan);
  if (!post_applied)
    return post_applied;

  if (!rex::codegen::ManifestConfig::WriteSdkVersionStamp(manifest_path, current_version)) {
    REXLOG_WARN("Failed to stamp manifest sdkVersion; next run may re-prompt");
  }

  return rex::Ok();
}

}  // namespace rexglue::cli
