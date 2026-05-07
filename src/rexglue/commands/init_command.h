/**
 * @file        rexglue/commands/init_command.h
 * @brief       Project initialization command interface
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "../cli_utils.h"

#include <string>

#include <rex/result.h>

namespace rexglue::cli {

using rex::Result;

/**
 * Options for the init command
 */
struct InitOptions {
  std::string project_name;  ///< Project name (required, becomes [project].name)
  std::string xex_path;      ///< Path to entrypoint XEX (required)
  std::string game_root;     ///< Game asset root for DLL guest-path derivation.
                             ///< Defaults to the directory containing xex_path.
  std::string project_root;  ///< Where to create the project. Defaults to CWD.
  bool scan_dlls = false;    ///< --dll: scan game_root for .dll files and add as [[modules]]
  std::string template_dir;  ///< Optional custom template directory for overrides
  bool force = false;        ///< Overwrite existing non-empty directory
};

/**
 * Initialize a new rexglue project
 * @param opts Init options
 * @param ctx CLI context
 * @return Success or error
 */
Result<void> InitProject(const InitOptions& opts, const CliContext& ctx);

struct InitModuleOptions {
  std::string app_root;    // Project root (must contain a manifest)
  std::string xex_path;    // Path to DLL XEX file
  std::string guest_path;  // Guest path for XexLoadImage matching
};

/**
 * Add a DLL module to an existing multi-binary project
 * @param opts Init module options
 * @param ctx CLI context
 * @return Success or error
 */
Result<void> InitModule(const InitModuleOptions& opts, const CliContext& ctx);

}  // namespace rexglue::cli
