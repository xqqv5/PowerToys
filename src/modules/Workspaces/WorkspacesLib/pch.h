// pch.h: This is a precompiled header file.
// Files listed below are compiled only once, improving build performance for future builds.
// This also affects IntelliSense performance, including code completion and many code browsing features.
// However, files listed here are ALL re-compiled if any one of them is updated between builds.
// Do not add files here that you will be updating frequently as this negates the performance advantage.

#ifndef PCH_H
#define PCH_H
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

// Include windows headers first 
#include <windows.h>

// Include WinRT headers
#include <winrt/base.h>

// Include Shell API headers after WinRT to avoid IUnknown conflicts
#include <shellapi.h>

// Standard library includes
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <optional>

// Common includes
#include <common/logger/logger.h>
#include <ProjectTelemetry.h>

#endif //PCH_H
