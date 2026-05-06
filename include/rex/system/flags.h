/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/cvar.h>

REXCVAR_DECLARE_EXTERN(bool, headless);
REXCVAR_DECLARE_EXTERN(bool, log_high_frequency_kernel_calls);
REXCVAR_DECLARE_EXTERN(bool, xex_apply_patches);
REXCVAR_DECLARE_EXTERN(uint32_t, license_mask);
REXCVAR_DECLARE_EXTERN(uint32_t, user_country);
REXCVAR_DECLARE_EXTERN(uint32_t, user_language);
REXCVAR_DECLARE_EXTERN(bool, kernel_pix);
REXCVAR_DECLARE_EXTERN(std::string, cl);
REXCVAR_DECLARE_EXTERN(bool, kernel_debug_monitor);
REXCVAR_DECLARE_EXTERN(bool, kernel_cert_monitor);
REXCVAR_DECLARE_EXTERN(bool, ignore_thread_priorities);
REXCVAR_DECLARE_EXTERN(bool, ignore_thread_affinities);
// writable_executable_memory lives in rexcore (STATIC); plain DECLARE here
// works at link time. The duplicate-storage concern across DLL boundaries
// applies to all of rexcore and is tracked separately.
REXCVAR_DECLARE(bool, writable_executable_memory);
REXCVAR_DECLARE_EXTERN(bool, protect_zero);
REXCVAR_DECLARE_EXTERN(bool, protect_on_release);
REXCVAR_DECLARE_EXTERN(bool, scribble_heap);
