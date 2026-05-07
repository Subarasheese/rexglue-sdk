/**
 * @file        rexglue/commands/upgrade_detect.cpp
 * @brief       Detect SDK-managed files needing overwrite on codegen upgrade
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "upgrade_detect.h"
#include "template_utils.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <rex/codegen/template_registry.h>

namespace rexglue::cli {

namespace fs = std::filesystem;

namespace {

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool IsUnderGeneratedTree(const fs::path& project_root, const fs::path& target) {
  std::error_code ec;
  auto rel = fs::relative(target, project_root, ec);
  if (ec)
    return false;
  auto first = rel.begin();
  return first != rel.end() && first->generic_string() == "generated";
}

// Returns true iff `c` is a character that can appear inside a path token in
// a cmake invocation (filename or path-separator characters). Anchoring the
// match this way avoids rewriting `mygame_config.toml.bak` or comments that
// happen to contain the legacy filename inside a longer string.
bool IsPathTokenChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' ||
         c == '/' || c == '\\';
}

bool ReplaceAll(std::string& haystack, std::string_view needle, std::string_view replacement) {
  if (needle.empty())
    return false;
  bool replaced = false;
  std::string::size_type pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    bool boundary_before = (pos == 0) || !IsPathTokenChar(haystack[pos - 1]);
    bool boundary_after =
        (pos + needle.size() >= haystack.size()) || !IsPathTokenChar(haystack[pos + needle.size()]);
    if (!boundary_before || !boundary_after) {
      pos += needle.size();
      continue;
    }
    haystack.replace(pos, needle.size(), replacement);
    pos += replacement.size();
    replaced = true;
  }
  return replaced;
}

bool IsSourceExtension(const fs::path& p) {
  static const std::unordered_set<std::string> kExts = {".cpp", ".cc",  ".cxx", ".c",  ".h",
                                                        ".hh",  ".hpp", ".hxx", ".inl"};
  auto ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return kExts.contains(ext);
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string ExtractIncludeBasename(std::string_view target) {
  auto pos = target.find_last_of("/\\");
  return std::string(pos == std::string_view::npos ? target : target.substr(pos + 1));
}

}  // namespace

UpgradeDetect::UpgradeDetect() = default;

std::string UpgradeDetect::RenderRexglueCmake(std::string_view project_name,
                                              std::string_view sdk_version,
                                              std::string_view entrypoint_out_dir) {
  rex::codegen::TemplateRegistry registry;
  auto names = parse_app_name(std::string(project_name));
  nlohmann::json data = {
      {"names", names_to_json(names)},
      {"sdk_version", std::string(sdk_version)},
      {"entrypoint_out_dir", std::string(entrypoint_out_dir)},
  };
  return registry.render("init/rexglue_cmake", data.dump());
}

std::vector<OverwriteEntry> UpgradeDetect::Plan(const fs::path& project_root,
                                                std::string_view project_name,
                                                std::string_view sdk_version,
                                                std::string_view entrypoint_out_dir) {
  std::vector<OverwriteEntry> plan;

  // generated/rexglue.cmake: owned by the SDK, lives inside generated/, free
  // to delete and regenerate on demand. Always silent.
  fs::path rexglue_cmake = project_root / "generated" / "rexglue.cmake";
  std::string rendered = RenderRexglueCmake(project_name, sdk_version, entrypoint_out_dir);
  std::string on_disk = fs::exists(rexglue_cmake) ? ReadFile(rexglue_cmake) : std::string{};
  if (rendered != on_disk) {
    plan.push_back({rexglue_cmake, std::move(rendered), OverwriteAction::Write, /*silent=*/true,
                    fmt::format("regenerate SDK helper for v{}", sdk_version)});
  }

  return plan;
}

std::vector<OverwriteEntry> UpgradeDetect::ScanCmakeReferences(
    const fs::path& project_root, std::string_view legacy_config_filename,
    std::string_view manifest_filename) {
  std::vector<OverwriteEntry> entries;
  if (legacy_config_filename.empty() || manifest_filename.empty() ||
      legacy_config_filename == manifest_filename) {
    return entries;
  }

  std::error_code ec;
  fs::recursive_directory_iterator it(project_root, fs::directory_options::skip_permission_denied,
                                      ec);
  if (ec) {
    return entries;
  }
  fs::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    if (it->is_directory()) {
      // Skip generated/ wholesale; anything inside is owned by the SDK and
      // gets regenerated by other plan entries.
      if (IsUnderGeneratedTree(project_root, it->path())) {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (!it->is_regular_file())
      continue;

    auto name = it->path().filename().string();
    auto ext = it->path().extension().string();
    bool is_cmake_file = (name == "CMakeLists.txt") || (ext == ".cmake");
    if (!is_cmake_file)
      continue;

    std::string content = ReadFile(it->path());
    if (content.empty())
      continue;
    std::string updated = content;
    if (!ReplaceAll(updated, legacy_config_filename, manifest_filename))
      continue;
    if (updated == content)
      continue;

    entries.push_back(
        {it->path(), std::move(updated), OverwriteAction::Write, /*silent=*/false,
         fmt::format("update reference from {} to {}", legacy_config_filename, manifest_filename)});
  }
  return entries;
}

std::vector<OverwriteEntry> UpgradeDetect::ScanSourceIncludeRewrites(
    const fs::path& project_root, std::string_view project_name) {
  std::vector<OverwriteEntry> entries;
  if (project_name.empty())
    return entries;

  auto names = parse_app_name(std::string(project_name));
  std::string old_basename_lc = ToLower(names.snake_case + "_config.h");
  std::string new_basename = names.snake_case + "_init.h";
  std::string new_basename_lc = ToLower(new_basename);

  static const std::regex include_re(R"(^(\s*#\s*include\s*[<"])([^>"]+)([>"].*)$)");

  std::error_code ec;
  fs::recursive_directory_iterator it(project_root, fs::directory_options::skip_permission_denied,
                                      ec);
  if (ec)
    return entries;
  fs::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    if (it->is_directory()) {
      if (IsUnderGeneratedTree(project_root, it->path()))
        it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file())
      continue;
    if (!IsSourceExtension(it->path()))
      continue;

    std::string content = ReadFile(it->path());
    if (content.empty())
      continue;

    // Split on '\n'; preserve any trailing '\r' on each line so we can rebuild
    // the file with its original line-ending convention.
    std::vector<std::string> lines;
    {
      std::string buf;
      for (char c : content) {
        if (c == '\n') {
          lines.push_back(std::move(buf));
          buf.clear();
        } else {
          buf.push_back(c);
        }
      }
      if (!buf.empty())
        lines.push_back(std::move(buf));
    }
    bool has_trailing_newline = !content.empty() && content.back() == '\n';

    auto extract_target = [&](const std::string& line) -> std::optional<std::string> {
      std::smatch m;
      if (!std::regex_search(line, m, include_re))
        return std::nullopt;
      return m[2].str();
    };

    bool has_init_include = false;
    for (const auto& line : lines) {
      auto target = extract_target(line);
      if (!target)
        continue;
      if (ToLower(ExtractIncludeBasename(*target)) == new_basename_lc) {
        has_init_include = true;
        break;
      }
    }

    bool changed = false;
    std::string out;
    out.reserve(content.size());
    for (size_t li = 0; li < lines.size(); ++li) {
      const std::string& line = lines[li];
      auto target = extract_target(line);
      auto append_line = [&](std::string_view payload) {
        out.append(payload);
        if (li + 1 < lines.size() || has_trailing_newline)
          out.push_back('\n');
      };

      if (!target || ToLower(ExtractIncludeBasename(*target)) != old_basename_lc) {
        append_line(line);
        continue;
      }

      if (has_init_include) {
        // Drop the stale line entirely (including its line ending).
        changed = true;
        continue;
      }

      // Rewrite the basename in-place; preserve path prefix and surrounding
      // formatting so a path like "generated/foo_config.h" becomes
      // "generated/foo_init.h" without churning the rest of the line.
      std::smatch m;
      if (!std::regex_search(line, m, include_re)) {
        append_line(line);
        continue;
      }
      std::string prefix = m[1].str();
      std::string old_target = m[2].str();
      std::string suffix = m[3].str();
      std::string base = ExtractIncludeBasename(old_target);
      std::string new_target = old_target;
      new_target.replace(new_target.size() - base.size(), base.size(), new_basename);
      append_line(prefix + new_target + suffix);
      has_init_include = true;
      changed = true;
    }

    if (!changed)
      continue;

    entries.push_back({it->path(), std::move(out), OverwriteAction::Write, /*silent=*/false,
                       fmt::format("update #include from {}_config.h to {}_init.h",
                                   names.snake_case, names.snake_case)});
  }
  return entries;
}

}  // namespace rexglue::cli
