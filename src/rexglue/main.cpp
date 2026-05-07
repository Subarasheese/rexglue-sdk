/**
 * @file        rexglue/main.cpp
 * @brief       ReXGlue CLI tool entry point
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include "cli_utils.h"
#include "commands/codegen_command.h"
#include "commands/init_command.h"
#include "commands/test_recompiler.h"

#include <chrono>
#include <iostream>
#include <map>
#include <sstream>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/result.h>
#include <rex/version.h>

// Codegen flags (definitions in codegen_flags.cpp)
REXCVAR_DECLARE(bool, force);
REXCVAR_DECLARE(bool, enable_exception_handlers);
REXCVAR_DEFINE_STRING(target, "", "Codegen",
                      "Comma-separated target modules for multi-binary codegen");

// Recompile-tests flags
REXCVAR_DEFINE_STRING(bin_dir, "", "RecompileTests",
                      "Directory containing linked .bin and .map files");
REXCVAR_DEFINE_STRING(asm_dir, "", "RecompileTests",
                      "Directory containing .s assembly source files");
REXCVAR_DEFINE_STRING(output, "", "RecompileTests", "Output path for recompile-tests");

// Init flags
REXCVAR_DEFINE_STRING(project_name, "", "Init",
                      "Project name (becomes [project].name in the manifest)");
REXCVAR_DEFINE_STRING(project_root, "", "Init",
                      "Where to create the project (defaults to current directory)");
REXCVAR_DEFINE_STRING(xex_path, "", "Init", "Path to entrypoint XEX (e.g. assets/Default.xex)");
REXCVAR_DEFINE_STRING(game_root, "", "Init",
                      "Game asset root for DLL guest-path derivation "
                      "(defaults to the directory containing --xex_path)");
REXCVAR_DEFINE_BOOL(scan_dll, false, "Init",
                    "Scan --game_root for .dll files and add each as a [[modules]] entry");
REXCVAR_DEFINE_STRING(template_dir, "", "Init", "Custom template directory for overrides");

// Init module flags (init module subcommand)
REXCVAR_DEFINE_STRING(guest_path, "", "InitModule", "Guest path for XexLoadImage matching");

using rex::Ok;
using rex::Result;

std::string GetTitleString() {
  return fmt::format("ReXGlue v{} - Xbox 360 Recompilation Toolkit", REXGLUE_VERSION_STRING);
}

void PrintUsage() {
  std::cerr << GetTitleString() + "\n\n";
  std::cerr << "Usage: rexglue <command> [flags] [args]\n\n";
  std::cerr << "Commands:\n";
  std::cerr << "  codegen <config.toml>   Analyze XEX and generate C++ code\n";
  std::cerr << "  init                    Initialize a new project\n";
  std::cerr << "  init module             Add a DLL module to an existing project\n";
  std::cerr << "  recompile-tests         Generate Catch2 tests from PPC assembly\n\n";
  std::cerr << "Init flags:\n";
  std::cerr << "  --project_name=<name>   Project name (required)\n";
  std::cerr << "  --xex_path=<path>       Path to entrypoint XEX (required)\n";
  std::cerr << "  --game_root=<dir>       Asset root for DLL guest-path derivation\n";
  std::cerr << "                          (default: directory containing --xex_path)\n";
  std::cerr << "  --scan_dll              Scan --game_root for .dll files and add as modules\n";
  std::cerr << "  --project_root=<dir>    Where to create the project (default: cwd)\n\n";
  std::cerr << "Codegen flags:\n";
  std::cerr
      << "  --target=a,b            Build specific DLL modules (entrypoint always included)\n\n";
  std::cerr << "Run 'rexglue --help' for flag details.\n";
}

int main(int argc, char** argv) {
  // Extract positional (non-flag) args from argv directly for command routing.
  // CLI11's remaining() behavior for positional args that appear before --flags
  // is version-dependent; bare words like "module" can be silently dropped.
  std::string command;
  std::string subcommand;
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] != '-') {
      if (command.empty())
        command = argv[i];
      else if (subcommand.empty())
        subcommand = argv[i];
      else
        break;
    }
  }

  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

  if (command.empty()) {
    PrintUsage();
    return 1;
  }

  // Set up logging from CVARs
  std::string level_str = REXCVAR_GET(log_level);
  std::string log_file_path = REXCVAR_GET(log_file);
  bool verbose = REXCVAR_GET(log_verbose);

  // Verbose overrides level if not explicitly set
  if (verbose && level_str == "info") {
    level_str = "trace";
    rex::cvar::SetFlagByName("log_level", "trace");
  }

  std::map<std::string, std::string> category_levels;
  auto log_config = rex::BuildLogConfig(log_file_path.empty() ? nullptr : log_file_path.c_str(),
                                        level_str, category_levels);
  log_config.log_to_console = true;  // CLI always logs to console
  rex::InitLogging(log_config);

  // Register callback for runtime level changes
  rex::RegisterLogLevelCallback();

  REXLOG_INFO(GetTitleString());

  rexglue::cli::CliContext ctx;
  ctx.verbose = verbose;
  bool force_flag = REXCVAR_GET(force);
  ctx.overwrite_existing = force_flag;
  ctx.generate_despite_errors = force_flag;
  ctx.skip_upgrade_consent = force_flag;
  ctx.enableExceptionHandlers = REXCVAR_GET(enable_exception_handlers);

  // Parse --target comma-separated list
  std::string target_str = REXCVAR_GET(target);
  if (!target_str.empty()) {
    std::istringstream ss(target_str);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
      if (!tok.empty())
        ctx.targets.push_back(tok);
    }
  }

  auto startTime = std::chrono::steady_clock::now();

  Result<void> result = Ok();
  if (command == "init" && subcommand == "module") {
    rexglue::cli::InitModuleOptions opts;
    opts.app_root = REXCVAR_GET(project_root);
    opts.xex_path = REXCVAR_GET(xex_path);
    opts.guest_path = REXCVAR_GET(guest_path);

    if (opts.app_root.empty() || opts.xex_path.empty() || opts.guest_path.empty()) {
      REXLOG_ERROR("--project_root, --xex_path, and --guest_path are required for 'init module'");
      return 1;
    }

    result = rexglue::cli::InitModule(opts, ctx);
  } else if (command == "init") {
    rexglue::cli::InitOptions opts;
    opts.project_name = REXCVAR_GET(project_name);
    opts.project_root = REXCVAR_GET(project_root);
    opts.xex_path = REXCVAR_GET(xex_path);
    opts.game_root = REXCVAR_GET(game_root);
    opts.scan_dlls = REXCVAR_GET(scan_dll);
    opts.template_dir = REXCVAR_GET(template_dir);
    opts.force = ctx.overwrite_existing;

    if (opts.project_name.empty()) {
      REXLOG_ERROR("--project_name is required for init command");
      return 1;
    }
    if (opts.xex_path.empty()) {
      REXLOG_ERROR("--xex_path is required for init command (path to entrypoint XEX)");
      return 1;
    }

    result = rexglue::cli::InitProject(opts, ctx);
  } else if (command == "codegen") {
    if (remaining.size() > 2) {
      REXLOG_ERROR("Too many arguments for codegen command");
      return 1;
    }
    std::string config_path;
    if (remaining.size() == 2) {
      config_path = remaining[1];
    } else {
      auto discovered = rexglue::cli::DiscoverManifestInCwd();
      if (!discovered) {
        REXLOG_ERROR("{}. Usage: rexglue codegen [config.toml]", discovered.error().what());
        return 1;
      }
      config_path = *discovered;
      REXLOG_INFO("Using manifest: {}", config_path);
    }
    result = rexglue::cli::CodegenFromConfig(config_path, ctx);
  } else if (command == "recompile-tests") {
    std::string bin_dir = REXCVAR_GET(bin_dir);
    std::string asm_dir = REXCVAR_GET(asm_dir);
    std::string output = REXCVAR_GET(output);

    if (bin_dir.empty() || asm_dir.empty() || output.empty()) {
      REXLOG_ERROR("--bin-dir, --asm-dir, and --output are required");
      return 1;
    }

    if (!rexglue::commands::recompile_tests(bin_dir, asm_dir, output)) {
      REXLOG_ERROR("Test recompilation failed");
      return 1;
    }
  } else {
    REXLOG_ERROR("Unknown command: {}", command);
    PrintUsage();
    return 1;
  }

  auto endTime = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

  if (!result) {
    if (result.error().category == rex::ErrorCategory::UserAbort) {
      REXLOG_INFO("{}", result.error().what());
      return 2;
    }
    REXLOG_ERROR("Operation failed: {} (took {:.3f}s)", result.error().what(),
                 elapsed.count() / 1000.0);
    return 1;
  }

  REXLOG_INFO("Operation completed successfully in {:.3f}s", elapsed.count() / 1000.0);
  return 0;
}
