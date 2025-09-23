#include "pch.h"
#include "WorkspacesService.h"

#include <filesystem>
#include <algorithm>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.ApplicationModel.Core.h>

// Local includes
#include "PwaHelper.h"
#include "WindowUtils.h"
#include "JsonUtils.h"
#include "WorkspacesData.h"
#include "IPCHelper.h"
#include "AppUtils.h"
#include "LaunchingStatus.h"

// workspaces-common includes
#include "../workspaces-common/MonitorUtils.h"
#include "../workspaces-common/WindowEnumerator.h"
#include "../workspaces-common/WindowFilter.h"
#include "../workspaces-common/WindowUtils.h"

// common includes
#include <common/utils/process_path.h>
#include <common/utils/winapi_error.h>
#include <common/logger/logger.h>

#include <filesystem>
#include <algorithm>

namespace
{
    const int WINDOW_SEARCH_TIMEOUT_MS = 3000;
    const int WINDOW_SEARCH_INTERVAL_MS = 50;
}

WorkspacesService::WorkspacesService()
{
    // Initialize apps cache time to a very old time point to force immediate refresh
    m_appsCacheTime = std::chrono::steady_clock::time_point{};
    Logger::info(L"WorkspacesService created");
}

WorkspacesService::~WorkspacesService()
{
    Stop();
    Logger::info(L"WorkspacesService destroyed");
}

void WorkspacesService::Start()
{
    std::lock_guard<std::mutex> lock(m_serviceMutex);

    if (m_enabled)
    {
        Logger::warn(L"WorkspacesService already started");
        return;
    }

    Logger::info(L"Starting WorkspacesService");

    // Start IPC service
    m_ipcHelper = std::make_unique<IPCHelper>(
        IPCHelperStrings::WorkspacesServicePipeName,
        L"", // No send pipe needed
        [this](const std::wstring& message) {
            OnIPCMessage(message);
        });

    m_enabled = true;
    m_shouldStop = false;

    // Pre-load and cache all apps at startup for intelligent window protection
    Logger::info(L"Pre-loading apps cache at startup");
    auto startCacheTime = std::chrono::high_resolution_clock::now();
    GetCachedAppsList(); // This will populate the cache
    auto endCacheTime = std::chrono::high_resolution_clock::now();
    auto cacheDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endCacheTime - startCacheTime);
    Logger::info(L"Apps cache pre-loaded in {} ms with {} entries", cacheDuration.count(), m_cachedAppsList.size());

    Logger::info(L"WorkspacesService started successfully");
}

void WorkspacesService::Stop()
{
    std::lock_guard<std::mutex> lock(m_serviceMutex);

    if (!m_enabled)
    {
        return;
    }

    Logger::info(L"Stopping WorkspacesService");

    m_shouldStop = true;
    m_enabled = false;

    // Clean up IPC
    m_ipcHelper.reset();

    Logger::info(L"WorkspacesService stopped");
}

void WorkspacesService::OnIPCMessage(const std::wstring& message)
{
    Logger::info(L"Received IPC message: {}", message);

    // Check if already processing a request (single request processing)
    bool expected = false;
    if (!m_processing.compare_exchange_strong(expected, true))
    {
        Logger::warn(L"Already processing a workspace request, ignoring: {}", message);
        return;
    }

    try
    {
        ProcessWorkspace(message);
    }
    catch (const std::exception& e)
    {
        Logger::error(L"Error processing workspace");
        Logger::error("Exception details: {}", e.what());
    }
    catch (...)
    {
        Logger::error(L"Unknown error processing workspace");
    }

    m_processing = false;
}

void WorkspacesService::ProcessWorkspace(const std::wstring& workspaceId)
{
    auto startTime = std::chrono::high_resolution_clock::now();
    Logger::info(L"Processing workspace: {}", workspaceId);

    // Load workspace data
    WorkspacesData::WorkspacesProject workspace;
    LoadWorkspace(workspaceId, workspace);

    if (workspace.id.empty())
    {
        Logger::error(L"Failed to load workspace: {}", workspaceId);
        return;
    }

    ExecuteWorkspaceSequence(workspace);

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    Logger::info(L"Workspace processing completed in {} ms", duration.count());
}

void WorkspacesService::ExecuteWorkspaceSequence(const WorkspacesData::WorkspacesProject& workspace)
{
    Logger::info(L"Executing workspace sequence for: {}", workspace.name);
    auto startTime = std::chrono::high_resolution_clock::now();

    // Initialize internal state for this workspace
    m_launchingStatus = std::make_unique<LaunchingStatus>(workspace);
    m_windowsBefore = WindowEnumerator::Enumerate(WindowFilter::Filter);
    m_monitors = MonitorUtils::IdentifyMonitors();

    {
        std::lock_guard lock(m_launchErrorsMutex);
        m_launchErrors.clear();
    }

    // Phase 1: Minimize unmanaged windows first
    Logger::info(L"Phase 1: Minimizing unmanaged windows");
    auto minimizeStart = std::chrono::high_resolution_clock::now();
    std::vector<HWND> emptyMovedWindows; // Initially no windows are moved
    MinimizeUnmanagedWindowsParallel(emptyMovedWindows);
    auto minimizeEnd = std::chrono::high_resolution_clock::now();
    auto minimizeDuration = std::chrono::duration_cast<std::chrono::milliseconds>(minimizeEnd - minimizeStart);
    Logger::info(L"Minimization completed in {} ms", minimizeDuration.count());

    // Phase 2: Move existing windows if enabled
    std::vector<HWND> movedWindows;
    if (workspace.moveExistingWindows || true)
    {
        Logger::info(L"Phase 2: Moving existing windows");
        auto moveStart = std::chrono::high_resolution_clock::now();
        movedWindows = ProcessExistingWindows(workspace);
        auto moveEnd = std::chrono::high_resolution_clock::now();
        auto moveDuration = std::chrono::duration_cast<std::chrono::milliseconds>(moveEnd - moveStart);
        Logger::info(L"Moved {} existing windows in {} ms", movedWindows.size(), moveDuration.count());
    }

    // Phase 3: Launch missing applications
    Logger::info(L"Phase 3: Launching missing applications");
    auto launchStart = std::chrono::high_resolution_clock::now();
    LaunchMissingApplicationsAdvanced(workspace, movedWindows);
    auto launchEnd = std::chrono::high_resolution_clock::now();
    auto launchDuration = std::chrono::duration_cast<std::chrono::milliseconds>(launchEnd - launchStart);
    Logger::info(L"Application launch phase completed in {} ms", launchDuration.count());

    // Phase 4: Wait for and arrange newly launched windows
    Logger::info(L"Phase 4: Processing newly launched windows");
    auto arrangeStart = std::chrono::high_resolution_clock::now();
    ProcessNewWindowsAdvanced(workspace, movedWindows);
    auto arrangeEnd = std::chrono::high_resolution_clock::now();
    auto arrangeDuration = std::chrono::duration_cast<std::chrono::milliseconds>(arrangeEnd - arrangeStart);
    Logger::info(L"Window arrangement phase completed in {} ms", arrangeDuration.count());

    auto endTime = std::chrono::high_resolution_clock::now();
    auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    Logger::info(L"Workspace sequence completed successfully in {} ms, total moved: {}",
                 totalDuration.count(),
                 movedWindows.size());

    // Log any launch errors
    {
        std::lock_guard lock(m_launchErrorsMutex);
        if (!m_launchErrors.empty())
        {
            Logger::warn(L"Launch errors occurred: {} errors", m_launchErrors.size());
            for (const auto& [appName, error] : m_launchErrors)
            {
                Logger::error(L"  {}: {}", appName, error);
            }
        }
    }
}

std::vector<HWND> WorkspacesService::ProcessExistingWindows(const WorkspacesData::WorkspacesProject& workspace)
{
    std::vector<HWND> movedWindows;
    Utils::PwaHelper pwaHelper;

    auto currentWindows = EnumerateCurrentWindows();
    Logger::info(L"Checking {} current windows for existing app matches", currentWindows.size());

    for (const auto& app : workspace.apps)
    {
        for (HWND window : currentWindows)
        {
            // Skip already moved windows
            if (std::find(movedWindows.begin(), movedWindows.end(), window) != movedWindows.end())
            {
                continue;
            }

            if (IsWindowMatchApp(window, app, pwaHelper))
            {
                Logger::info(L"Found existing window for app: {}", app.name);

                if (MoveWindowToPosition(window, app))
                {
                    movedWindows.push_back(window);
                    Logger::info(L"Successfully moved existing window for: {}", app.name);
                }
                break; // Only move the first matching window per application
            }
        }
    }

    return movedWindows;
}

void WorkspacesService::LaunchMissingApplications(const WorkspacesData::WorkspacesProject& workspace,
                                                  const std::vector<HWND>& existingWindows)
{
    Utils::PwaHelper pwaHelper;

    for (const auto& app : workspace.apps)
    {
        // Check if there's already an existing window
        bool hasExistingWindow = false;
        for (HWND window : existingWindows)
        {
            if (IsWindowMatchApp(window, app, pwaHelper))
            {
                hasExistingWindow = true;
                break;
            }
        }

        if (!hasExistingWindow)
        {
            Logger::info(L"Launching missing application: {}", app.name);
            LaunchApplication(app);
        }
        else
        {
            Logger::trace(L"Application {} already has existing window, skipping launch", app.name);
        }
    }
}

void WorkspacesService::LaunchMissingApplicationsAdvanced(const WorkspacesData::WorkspacesProject& workspace,
                                                          const std::vector<HWND>& existingWindows)
{
    const long maxWaitTimeMs = 2000; // Optimized from original 3000
    const long pollIntervalMs = 50; // Optimized from original 100

    Utils::PwaHelper pwaHelper;

    // Launch apps using advanced logic with proper sequencing
    for (auto appState = m_launchingStatus->GetNext(LaunchingState::Waiting);
         appState.has_value();
         appState = m_launchingStatus->GetNext(LaunchingState::Waiting))
    {
        auto app = appState.value().application;

        // Check if already has existing window
        bool hasExistingWindow = false;
        for (HWND window : existingWindows)
        {
            if (IsWindowMatchApp(window, app, pwaHelper))
            {
                hasExistingWindow = true;
                m_launchingStatus->Update(app, LaunchingState::LaunchedAndMoved);
                Logger::info(L"Application {} already has existing window", app.name);
                break;
            }
        }

        if (hasExistingWindow)
        {
            continue;
        }

        // Wait for previous instances of the same app to finish launching and moving
        long waitingTime = 0;
        bool additionalWait = false;
        while (!m_launchingStatus->AllInstancesOfTheAppLaunchedAndMoved(app) && waitingTime < maxWaitTimeMs)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
            waitingTime += pollIntervalMs;
            additionalWait = true;
        }

        // Special delay for Outlook to prevent launch issues
        if (additionalWait)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Optimized from 1000ms
        }

        if (waitingTime >= maxWaitTimeMs)
        {
            Logger::info(L"Wait time for launching next {} instance expired", app.name);
        }

        // Launch the application
        bool launched = false;
        {
            std::lock_guard lock(m_launchErrorsMutex);
            launched = LaunchAppWithFullLogic(app, m_launchErrors);
        }

        if (launched)
        {
            m_launchingStatus->Update(app, LaunchingState::Launched);
            Logger::info(L"Successfully launched {}", app.name);
        }
        else
        {
            Logger::error(L"Failed to launch {}", app.name);
            m_launchingStatus->Update(app, LaunchingState::Failed);
        }
    }

    Logger::info(L"Advanced app launching completed for {} apps", workspace.apps.size());
}

void WorkspacesService::ProcessNewWindows(const WorkspacesData::WorkspacesProject& workspace,
                                          std::vector<HWND>& movedWindows)
{
    Utils::PwaHelper pwaHelper;

    auto startTime = std::chrono::high_resolution_clock::now();

    // Wait for new windows to appear and move them
    while (true)
    {
        auto currentTime = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime);

        if (elapsed.count() > WINDOW_SEARCH_TIMEOUT_MS)
        {
            Logger::info(L"Window search timeout reached");
            break;
        }

        bool foundNewWindow = false;
        auto currentWindows = EnumerateCurrentWindows();

        for (const auto& app : workspace.apps)
        {
            // Check if a window for this app has already been moved
            bool appAlreadyMoved = false;
            for (HWND movedWindow : movedWindows)
            {
                if (IsWindowMatchApp(movedWindow, app, pwaHelper))
                {
                    appAlreadyMoved = true;
                    break;
                }
            }

            if (appAlreadyMoved)
            {
                continue;
            }

            // Find new windows
            for (HWND window : currentWindows)
            {
                if (std::find(movedWindows.begin(), movedWindows.end(), window) != movedWindows.end())
                {
                    continue;
                }

                if (IsWindowMatchApp(window, app, pwaHelper))
                {
                    Logger::info(L"Found new window for app: {}", app.name);

                    if (MoveWindowToPosition(window, app))
                    {
                        movedWindows.push_back(window);
                        foundNewWindow = true;
                        Logger::info(L"Successfully moved new window for: {}", app.name);
                    }
                    break;
                }
            }
        }

        if (!foundNewWindow)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(WINDOW_SEARCH_INTERVAL_MS));
        }
    }
}

void WorkspacesService::ProcessNewWindowsAdvanced(const WorkspacesData::WorkspacesProject& workspace,
                                                  std::vector<HWND>& movedWindows)
{
    const long maxWaitTimeMs = 5000; // Extended timeout for comprehensive window search
    const long pollIntervalMs = 50; // Frequent polling for responsiveness

    Utils::PwaHelper pwaHelper;
    auto startTime = std::chrono::high_resolution_clock::now();

    Logger::info(L"Starting advanced window processing with {} second timeout", maxWaitTimeMs / 1000);

    while (true)
    {
        auto currentTime = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime);

        if (elapsed.count() > maxWaitTimeMs)
        {
            Logger::info(L"Advanced window search timeout reached");
            break;
        }

        // Check if all apps are launched and moved
        if (m_launchingStatus->AllLaunchedAndMoved())
        {
            Logger::info(L"All applications launched and moved, finishing early");
            break;
        }

        bool foundNewWindow = false;
        auto currentWindows = EnumerateCurrentWindows();

        // Process each application that needs a window
        for (const auto& app : workspace.apps)
        {
            auto appState = m_launchingStatus->Get(app);
            if (!appState.has_value() || appState.value().state != LaunchingState::Launched)
            {
                continue; // Skip apps that haven't been launched yet
            }

            // Find best matching window for this app
            auto bestWindow = GetNearestWindow(app, movedWindows, pwaHelper);
            if (bestWindow.has_value())
            {
                HWND window = bestWindow.value().window;
                Logger::info(L"Found new window for app: {} (distance: {})", app.name, bestWindow.value().distance);

                if (MoveWindowWithStateHandling(window, app))
                {
                    movedWindows.push_back(window);
                    foundNewWindow = true;

                    // Update launching status
                    m_launchingStatus->Update(app, window, LaunchingState::LaunchedAndMoved);
                    Logger::info(L"Successfully moved new window for: {}", app.name);
                }
                else
                {
                    Logger::warn(L"Failed to move window for: {}", app.name);
                    m_launchingStatus->Update(app, window, LaunchingState::Failed);
                }
            }
        }

        if (!foundNewWindow)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
        }
    }

    Logger::info(L"Advanced window processing completed, total moved: {}", movedWindows.size());
}

void WorkspacesService::MinimizeUnmanagedWindows(const std::vector<HWND>& movedWindows)
{
    auto currentWindows = EnumerateCurrentWindows();
    int minimizedCount = 0;
    int protectedCount = 0;
    Utils::PwaHelper pwaHelper;

    // Get current workspace for app protection
    auto currentWorkspace = m_launchingStatus ? m_launchingStatus->GetWorkspace() : WorkspacesData::WorkspacesProject{};
    
    if (currentWorkspace.apps.empty())
    {
        Logger::warn(L"No workspace apps available for protection - this may cause issues");
    }

    for (HWND window : currentWindows)
    {
        // Skip already moved windows
        if (std::find(movedWindows.begin(), movedWindows.end(), window) != movedWindows.end())
        {
            continue;
        }

        // Skip system popups
        if (WindowFilter::FilterPopup(window))
        {
            continue;
        }

        // **CRITICAL: Check if window belongs to workspace apps before minimizing**
        if (IsWindowInAppList(window, currentWorkspace, pwaHelper))
        {
            protectedCount++;
            continue; // Don't minimize workspace app windows
        }

        // Minimize unmanaged window
        if (ShowWindow(window, SW_FORCEMINIMIZE))
        {
            minimizedCount++;
        }
    }

    Logger::info(L"Window management: {} minimized, {} protected as workspace apps", minimizedCount, protectedCount);
}

bool WorkspacesService::IsWindowMatchApp(HWND window, const WorkspacesData::WorkspacesProject::Application& app, Utils::PwaHelper& pwaHelper)
{
    std::wstring processPath = get_process_path(window);
    if (processPath.empty())
    {
        return false;
    }

    std::wstring processName = std::filesystem::path(processPath).stem();
    std::wstring windowAumid = Utils::GetAUMIDFromWindow(window);

    // Primary match: AUMID (most reliable)
    if (!windowAumid.empty() && !app.appUserModelId.empty() &&
        app.appUserModelId == windowAumid)
    {
        return true;
    }

    // Secondary match: Path matching
    if (app.path == processPath)
    {
        return true;
    }

    // Fallback match: Process name matching
    if (app.name == processName)
    {
        return true;
    }

    // PWA special handling
    if (!app.pwaAppId.empty())
    {
        std::wstring processNameLower = processName;
        std::transform(processNameLower.begin(), processNameLower.end(), processNameLower.begin(), ::towlower);

        if (processNameLower == L"msedge" || processNameLower == L"chrome")
        {
            std::optional<std::wstring> pwaAppId;
            if (processNameLower == L"msedge")
            {
                pwaAppId = pwaHelper.GetEdgeAppId(windowAumid);
            }
            else if (processNameLower == L"chrome")
            {
                pwaAppId = pwaHelper.GetChromeAppId(windowAumid);
            }

            if (pwaAppId.has_value() && pwaAppId.value() == app.pwaAppId)
            {
                return true;
            }
        }
    }

    return false;
}

bool WorkspacesService::MoveWindowToPosition(HWND window, const WorkspacesData::WorkspacesProject::Application& app)
{
    // Get current window position for logging
    RECT currentRect;
    GetWindowRect(window, &currentRect);
    
    RECT targetRect;
    targetRect.left = app.position.x;
    targetRect.top = app.position.y;
    targetRect.right = app.position.x + app.position.width;
    targetRect.bottom = app.position.y + app.position.height;

    // Get target monitor for DPI and work area calculations
    HMONITOR targetMonitor = MonitorFromRect(&targetRect, MONITOR_DEFAULTTOPRIMARY);

    Logger::info(L"Moving window - Current: ({},{}) {}x{}, Target: ({},{}) {}x{}", 
                currentRect.left, currentRect.top, 
                currentRect.right - currentRect.left, currentRect.bottom - currentRect.top,
                targetRect.left, targetRect.top, 
                targetRect.right - targetRect.left, targetRect.bottom - targetRect.top);

    // Handle minimized windows
    if (app.isMinimized)
    {
        if (!ShowWindow(window, SW_FORCEMINIMIZE))
        {
            DWORD error = GetLastError();
            Logger::error(L"ShowWindow minimize failed for {}: error {}", app.name, error);
            return false;
        }
        Logger::info(L"Successfully minimized window for {}", app.name);
        return true;
    }

    // Convert screen coordinates to work area coordinates for proper positioning
    MONITORINFOEXW monitorInfo{ sizeof(MONITORINFOEXW) };
    GetMonitorInfoW(targetMonitor, &monitorInfo);

    auto xOffset = monitorInfo.rcWork.left - monitorInfo.rcMonitor.left;
    auto yOffset = monitorInfo.rcWork.top - monitorInfo.rcMonitor.top;

    RECT adjustedRect = targetRect;
    adjustedRect.left -= xOffset;
    adjustedRect.right -= xOffset;
    adjustedRect.top -= yOffset;
    adjustedRect.bottom -= yOffset;

    // Handle maximized windows
    if (app.isMaximized)
    {
        // First ensure window is visible but not activated
        ShowWindow(window, SW_SHOWNOACTIVATE);
        
        // Move to correct position first
        BOOL moveResult = SetWindowPos(window, nullptr, 
            adjustedRect.left, adjustedRect.top, 
            adjustedRect.right - adjustedRect.left, adjustedRect.bottom - adjustedRect.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_DEFERERASE);
        
        if (!moveResult)
        {
            DWORD error = GetLastError();
            Logger::error(L"SetWindowPos move failed for {}: error {}", app.name, error);
            return false;
        }
        
        // Then maximize
        if (!ShowWindow(window, SW_MAXIMIZE))
        {
            DWORD error = GetLastError();
            Logger::error(L"ShowWindow maximize failed for {}: error {}", app.name, error);
            return false;
        }
        
        Logger::info(L"Successfully moved and maximized window for {}", app.name);
        return true;
    }

    // Handle normal windows
    // First ensure window is visible but not activated
    ShowWindow(window, SW_SHOWNOACTIVATE);
    
    // Move and resize window
    BOOL result = SetWindowPos(window, nullptr, 
        adjustedRect.left, adjustedRect.top, 
        adjustedRect.right - adjustedRect.left, adjustedRect.bottom - adjustedRect.top,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_DEFERERASE);

    if (!result)
    {
        DWORD error = GetLastError();
        Logger::error(L"SetWindowPos failed for {}: error {}", app.name, error);
        return false;
    }

    // Verify the move by checking final position
    RECT finalRect;
    GetWindowRect(window, &finalRect);
    Logger::info(L"Window moved for {} - Final position: ({},{}) {}x{}", 
                app.name, finalRect.left, finalRect.top, 
                finalRect.right - finalRect.left, finalRect.bottom - finalRect.top);

    return true;
}

bool WorkspacesService::IsWindowInAppList(HWND window, const WorkspacesData::WorkspacesProject& workspace, Utils::PwaHelper& pwaHelper)
{
    if (WindowFilter::FilterPopup(window))
    {
        return true; // Don't minimize system popups
    }

    std::wstring processPath = get_process_path(window);
    if (processPath.empty())
    {
        return false; // Allow minimization of windows without valid process path
    }

    // Enhanced app checking logic using AUMID
    std::wstring processName = std::filesystem::path(processPath).stem();
    std::wstring windowAumid = Utils::GetAUMIDFromWindow(window);

    // ONLY protect workspace apps - minimize everything else
    for (const auto& app : workspace.apps)
    {
        // Primary match: AppUserModelId (most reliable)
        if (!windowAumid.empty() && !app.appUserModelId.empty() && 
            app.appUserModelId == windowAumid) {
            Logger::trace(L"Window PROTECTED by workspace AUMID match: {} -> {}", processName, app.name);
            return true; // Is workspace app, don't minimize
        }

        // Secondary match: Direct path match
        if (app.path == processPath) {
            Logger::trace(L"Window PROTECTED by workspace path match: {} -> {}", processName, app.name);
            return true; // Is workspace app, don't minimize
        }

        // Fallback match: Process name
        if (app.name == processName) {
            Logger::trace(L"Window PROTECTED by workspace process name match: {} -> {}", processName, app.name);
            return true; // Is workspace app, don't minimize
        }

        // PWA app special handling
        if (!app.pwaAppId.empty())
        {
            std::wstring processNameLower = processName;
            std::transform(processNameLower.begin(), processNameLower.end(), processNameLower.begin(), ::towlower);
            
            if (processNameLower == L"msedge" || processNameLower == L"chrome")
            {
                std::optional<std::wstring> pwaAppId{};
                
                if (processNameLower == L"msedge")
                {
                    pwaAppId = pwaHelper.GetEdgeAppId(windowAumid);
                }
                else if (processNameLower == L"chrome")
                {
                    pwaAppId = pwaHelper.GetChromeAppId(windowAumid);
                }
                
                if (pwaAppId.has_value() && pwaAppId.value() == app.pwaAppId)
                {
                    Logger::trace(L"Window PROTECTED by workspace PWA match: {} -> {}", processName, app.name);
                    return true; // Is workspace PWA app, don't minimize
                }
            }
        }
    }

    // NOT a workspace app - can be minimized
    Logger::trace(L"Window NOT PROTECTED, will be minimized: {}", processName);
    return false; // Not a workspace app, minimize it
}

void WorkspacesService::LoadWorkspace(const std::wstring& workspaceId, WorkspacesData::WorkspacesProject& workspace)
{
    try
    {
        auto workspacesFile = WorkspacesData::WorkspacesFile();
        auto workspacesResult = JsonUtils::ReadWorkspaces(workspacesFile);

        if (workspacesResult.isOk())
        {
            auto workspaces = workspacesResult.getValue();
            for (const auto& ws : workspaces)
            {
                if (ws.id == workspaceId)
                {
                    workspace = ws;
                    Logger::info(L"Successfully loaded workspace: {} with {} apps", ws.name, ws.apps.size());
                    return;
                }
            }
        }
        else
        {
            Logger::error(L"Failed to read workspaces file: {}", static_cast<int>(workspacesResult.getError()));
        }

        auto tempWorkspacesFile = WorkspacesData::TempWorkspacesFile();
        auto tempResult = JsonUtils::ReadSingleWorkspace(tempWorkspacesFile);

        if (tempResult.isOk())
        {
            auto tempWorkspace = tempResult.getValue();
            if (tempWorkspace.id == workspaceId)
            {
                workspace = tempWorkspace;
                Logger::info(L"Successfully loaded workspace from temp file: {} with {} apps", tempWorkspace.name, tempWorkspace.apps.size());
                return;
            }
        }

        Logger::error(L"Workspace not found: {}", workspaceId);
    }
    catch (const std::exception& e)
    {
        Logger::error(L"Failed to load workspace");
        Logger::error("Exception details: {}", e.what());
    }
}

std::vector<HWND> WorkspacesService::EnumerateCurrentWindows()
{
    return WindowEnumerator::Enumerate(WindowFilter::Filter);
}

bool WorkspacesService::LaunchApplication(const WorkspacesData::WorkspacesProject::Application& app)
{
    try
    {
        Logger::info(L"Launching application: {} (path: {})", app.name, app.path);

        if (!app.packageFullName.empty())
        {
            Logger::info(L"Launching packaged app: {}", app.packageFullName);

            return true;
        }

        if (!app.path.empty())
        {
            Logger::info(L"Launching regular app from path: {}", app.path);

            STARTUPINFO si = { 0 };
            si.cb = sizeof(STARTUPINFO);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_SHOWMINNOACTIVE;

            PROCESS_INFORMATION pi = { 0 };

            std::wstring commandLine = L"\"" + app.path + L"\"";
            if (!app.commandLineArgs.empty())
            {
                commandLine += L" " + app.commandLineArgs;
            }

            std::wstring workingDir = std::filesystem::path(app.path).parent_path();

            if (CreateProcess(
                    app.path.c_str(), // Application name
                    commandLine.data(), // Command line (modifiable)
                    nullptr, // Process security attributes
                    nullptr, // Primary thread security attributes
                    FALSE, // Inherit handles
                    0, // Creation flags
                    nullptr, // Environment
                    workingDir.c_str(), // Current directory
                    &si, // Startup info
                    &pi)) // Process info
            {
                Logger::info(L"Successfully launched application using CreateProcess: {}", app.name);

                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);

                return true;
            }
            else
            {
                DWORD error = GetLastError();
                Logger::error(L"CreateProcess failed for {}: error code {}", app.name, error);

                Logger::info(L"Falling back to ShellExecuteEx for: {}", app.name);

                SHELLEXECUTEINFO sei = { 0 };
                sei.cbSize = sizeof(SHELLEXECUTEINFO);
                sei.hwnd = nullptr;
                sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
                sei.lpVerb = L"open";
                sei.lpFile = app.path.c_str();
                sei.lpParameters = app.commandLineArgs.empty() ? nullptr : app.commandLineArgs.c_str();
                sei.lpDirectory = workingDir.c_str();
                sei.nShow = SW_SHOWMINNOACTIVE;

                if (ShellExecuteEx(&sei))
                {
                    Logger::info(L"Successfully launched application using ShellExecuteEx: {}", app.name);

                    if (sei.hProcess)
                    {
                        CloseHandle(sei.hProcess);
                    }

                    return true;
                }
                else
                {
                    DWORD shellError = GetLastError();
                    Logger::error(L"ShellExecuteEx also failed for {}: error code {}", app.name, shellError);
                    return false;
                }
            }
        }

        Logger::error(L"Unable to launch application: no valid path or package info for {}", app.name);
        return false;
    }
    catch (const std::exception& e)
    {
        Logger::error(L"Exception launching application: {}", app.name);
        Logger::error("Exception details: {}", e.what());
        return false;
    }
    catch (...)
    {
        Logger::error(L"Unknown exception launching application: {}", app.name);
        return false;
    }
}

// Advanced methods implementation

const Utils::Apps::AppList& WorkspacesService::GetCachedAppsList()
{
    auto currentTime = std::chrono::steady_clock::now();

    // Check if cache is still valid
    if (currentTime - m_appsCacheTime < APPS_CACHE_DURATION && !m_cachedAppsList.empty())
    {
        Logger::trace(L"Using cached apps list with {} entries", m_cachedAppsList.size());
        return m_cachedAppsList;
    }

    // Refresh cache
    Logger::info(L"Refreshing apps cache");
    m_cachedAppsList = Utils::Apps::GetAppsList();
    m_appsCacheTime = currentTime;
    Logger::info(L"Apps cache refreshed with {} entries", m_cachedAppsList.size());

    return m_cachedAppsList;
}

// Helper function for launching apps (from AppLauncher.cpp)
namespace
{
    struct Result
    {
        bool success;
        std::wstring error;
    };

    Result LaunchApp(const std::wstring& appPath, const std::wstring& commandLineArgs, bool elevated)
    {
        std::wstring dir = std::filesystem::path(appPath).parent_path();

        // Use CreateProcess for faster startup when not elevated
        if (!elevated)
        {
            STARTUPINFO si = { 0 };
            si.cb = sizeof(STARTUPINFO);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_SHOWMINNOACTIVE;

            PROCESS_INFORMATION pi = { 0 };

            // Combine app path and command line args
            std::wstring commandLine = L"\"" + appPath + L"\" " + commandLineArgs;

            if (CreateProcess(
                    appPath.c_str(), // Application name
                    commandLine.data(), // Command line (modifiable)
                    nullptr, // Process security attributes
                    nullptr, // Primary thread security attributes
                    FALSE, // Inherit handles
                    0, // Creation flags
                    nullptr, // Environment
                    dir.c_str(), // Current directory
                    &si, // Startup info
                    &pi)) // Process info
            {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                return { true, L"" };
            }
            else
            {
                std::wstring error = get_last_error_or_default(GetLastError());
                Logger::error(L"Failed to launch process with CreateProcess. {}", error);
                // Fall through to ShellExecuteEx
            }
        }

        // Fallback to ShellExecuteEx for elevated processes or if CreateProcess failed
        SHELLEXECUTEINFO sei = { 0 };
        sei.cbSize = sizeof(SHELLEXECUTEINFO);
        sei.hwnd = nullptr;
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
        sei.lpVerb = elevated ? L"runas" : L"open";
        sei.lpFile = appPath.c_str();
        sei.lpParameters = commandLineArgs.c_str();
        sei.lpDirectory = dir.c_str();
        sei.nShow = SW_SHOWMINNOACTIVE;

        if (!ShellExecuteEx(&sei))
        {
            std::wstring error = get_last_error_or_default(GetLastError());
            Logger::error(L"Failed to launch process. {}", error);
            return { false, error };
        }

        if (sei.hProcess)
        {
            CloseHandle(sei.hProcess);
        }

        return { true, L"" };
    }

    bool LaunchPackagedApp(const std::wstring& packageFullName, ErrorList& launchErrors)
    {
        try
        {
            winrt::Windows::Management::Deployment::PackageManager packageManager;
            for (const auto& package : packageManager.FindPackagesForUser({}))
            {
                if (package.Id().FullName() == packageFullName)
                {
                    auto getAppListEntriesOperation = package.GetAppListEntriesAsync();
                    auto appEntries = getAppListEntriesOperation.get();

                    if (appEntries.Size() > 0)
                    {
                        winrt::Windows::Foundation::IAsyncOperation<bool> launchOperation = appEntries.GetAt(0).LaunchAsync();
                        bool launchResult = launchOperation.get();
                        return launchResult;
                    }
                    else
                    {
                        Logger::error(L"No app entries found for the package.");
                        launchErrors.push_back({ packageFullName, L"No app entries found for the package." });
                    }
                }
            }
        }
        catch (const winrt::hresult_error& ex)
        {
            std::wstring message = ex.message().c_str();
            Logger::error(L"WinRT exception encountered during app launch: {}", message);
            launchErrors.push_back({ packageFullName, message });
        }
        catch (const std::exception& ex)
        {
            std::wstring message = winrt::to_hstring(ex.what()).c_str();
            Logger::error(L"std::exception encountered during app launch: {}", message);
            launchErrors.push_back({ packageFullName, message });
        }
        catch (...)
        {
            Logger::error(L"Unknown exception encountered during app launch");
            launchErrors.push_back({ packageFullName, L"Unknown exception during app launch" });
        }
        return false;
    }
} // End anonymous namespace

bool WorkspacesService::LaunchAppWithFullLogic(const WorkspacesData::WorkspacesProject::Application& app,
                                               ErrorList& launchErrors)
{
    bool launched = false;

    // Constants for PWA handling
    const std::wstring EdgeFilename = L"msedge.exe";
    const std::wstring EdgePwaFilename = L"msedge_proxy.exe";
    const std::wstring ChromeFilename = L"chrome.exe";
    const std::wstring ChromePwaFilename = L"chrome_proxy.exe";
    const std::wstring PwaCommandLineAddition = L"--profile-directory=Default --app-id=";
    const std::wstring SteamProtocolPrefix = L"steam:";

    // Try packaged apps with protocol in registry first
    if (!app.packageFullName.empty())
    {
        // This would require RegistryUtils - simplified for now
        Logger::trace(L"Attempting packaged app launch for {}", app.name);

        // Try direct AppUserModelId launch
        if (!app.appUserModelId.empty())
        {
            Logger::trace(L"Launching {} as {}", app.name, app.appUserModelId);
            auto res = LaunchApp(L"shell:AppsFolder\\" + app.appUserModelId, app.commandLineArgs, app.isElevated);
            if (res.success)
            {
                launched = true;
            }
            else
            {
                launchErrors.push_back({ std::filesystem::path(app.path).filename(), res.error });
            }
        }
    }

    // Try Steam protocol launch
    if (!launched && !app.appUserModelId.empty() && app.appUserModelId.find(SteamProtocolPrefix) != std::wstring::npos)
    {
        Logger::trace(L"Launching {} as {}", app.name, app.appUserModelId);
        auto res = LaunchApp(app.appUserModelId, app.commandLineArgs, app.isElevated);
        if (res.success)
        {
            launched = true;
        }
        else
        {
            launchErrors.push_back({ std::filesystem::path(app.path).filename(), res.error });
        }
    }

    // Try packaged app launch without command line args
    if (!launched && !app.packageFullName.empty() && app.commandLineArgs.empty() && !app.isElevated)
    {
        Logger::trace(L"Launching packaged app {}", app.name);
        launched = LaunchPackagedApp(app.packageFullName, launchErrors);
    }

    // PWA app handling
    std::wstring appPathFinal = app.path;
    std::wstring commandLineArgsFinal = app.commandLineArgs;

    if (!launched && !app.pwaAppId.empty())
    {
        int version = 0;
        if (!app.version.empty())
        {
            try
            {
                version = std::stoi(app.version);
            }
            catch (...)
            {
                Logger::error(L"Invalid version format: {}", app.version);
                version = 0;
            }
        }

        // Try modern PWA launch first
        if (version >= 1 && !app.appUserModelId.empty())
        {
            auto res = LaunchApp(L"shell:AppsFolder\\" + app.appUserModelId, app.commandLineArgs, app.isElevated);
            if (res.success)
            {
                launched = true;
            }
            else
            {
                launchErrors.push_back({ app.appUserModelId, res.error });
            }
        }

        // Fallback to PWA proxy launch
        if (!launched)
        {
            std::filesystem::path appPath(app.path);
            if (appPath.filename() == EdgeFilename)
            {
                appPathFinal = appPath.parent_path() / EdgePwaFilename;
                commandLineArgsFinal = PwaCommandLineAddition + app.pwaAppId + L" " + app.commandLineArgs;
            }
            else if (appPath.filename() == ChromeFilename)
            {
                appPathFinal = appPath.parent_path() / ChromePwaFilename;
                commandLineArgsFinal = PwaCommandLineAddition + app.pwaAppId + L" " + app.commandLineArgs;
            }
        }
    }

    // Final fallback: regular executable launch
    if (!launched)
    {
        Logger::trace(L"Launching {} at {}", app.name, appPathFinal);

        DWORD dwAttrib = GetFileAttributesW(appPathFinal.c_str());
        if (dwAttrib == INVALID_FILE_ATTRIBUTES)
        {
            Logger::error(L"File not found at {}", appPathFinal);
            launchErrors.push_back({ std::filesystem::path(appPathFinal).filename(), L"File not found" });
            return false;
        }

        auto res = LaunchApp(appPathFinal, commandLineArgsFinal, app.isElevated);
        if (res.success)
        {
            launched = true;
        }
        else
        {
            launchErrors.push_back({ std::filesystem::path(appPathFinal).filename(), res.error });
        }
    }

    Logger::trace(L"{} {} at {}", app.name, (launched ? L"launched" : L"not launched"), appPathFinal);
    return launched;
}

std::optional<WindowWithDistance> WorkspacesService::GetNearestWindow(
    const WorkspacesData::WorkspacesProject::Application& app,
    const std::vector<HWND>& movedWindows,
    Utils::PwaHelper& pwaHelper)
{
    WindowWithDistance nearestWindowWithDistance{};
    bool foundMatch = false;

    // Distance calculation helper
    auto calculateDistance = [&app](HWND window) -> int {
        WINDOWPLACEMENT placement{};
        ::GetWindowPlacement(window, &placement);

        if (app.isMinimized && (placement.showCmd == SW_SHOWMINIMIZED))
        {
            return 0; // Perfect match for minimized
        }

        int placementPenalty = 1; // Both normal
        if (app.isMinimized || (placement.showCmd == SW_SHOWMINIMIZED))
        {
            placementPenalty = 10000; // One minimized, one not
        }

        RECT windowPosition;
        GetWindowRect(window, &windowPosition);

        return placementPenalty +
               abs(app.position.x - windowPosition.left) +
               abs(app.position.y - windowPosition.top) +
               abs(app.position.x + app.position.width - windowPosition.right) +
               abs(app.position.y + app.position.height - windowPosition.bottom);
    };

    auto currentWindows = EnumerateCurrentWindows();
    for (HWND window : currentWindows)
    {
        if (WindowFilter::FilterPopup(window))
        {
            continue;
        }

        if (std::find(movedWindows.begin(), movedWindows.end(), window) != movedWindows.end())
        {
            continue;
        }

        std::wstring processPath = get_process_path(window);
        if (processPath.empty())
        {
            continue;
        }

        // Enhanced matching logic using AUMID
        std::wstring processName = std::filesystem::path(processPath).stem();
        std::wstring windowAumid = Utils::GetAUMIDFromWindow(window);

        // Primary match: AppUserModelId (most reliable)
        bool isMatch = (!windowAumid.empty() && !app.appUserModelId.empty() &&
                        app.appUserModelId == windowAumid);

        // Secondary match: Direct path match
        if (!isMatch && app.path == processPath)
        {
            isMatch = true;
        }

        // Fallback match: Process name
        if (!isMatch && app.name == processName)
        {
            isMatch = true;
        }

        // PWA app special handling
        if (!isMatch && !app.pwaAppId.empty())
        {
            std::wstring processNameLower = processName;
            std::transform(processNameLower.begin(), processNameLower.end(), processNameLower.begin(), ::towlower);

            if (processNameLower == L"msedge" || processNameLower == L"chrome")
            {
                std::optional<std::wstring> pwaAppId{};
                if (processNameLower == L"msedge")
                {
                    pwaAppId = pwaHelper.GetEdgeAppId(windowAumid);
                }
                else if (processNameLower == L"chrome")
                {
                    pwaAppId = pwaHelper.GetChromeAppId(windowAumid);
                }

                if (pwaAppId.has_value() && pwaAppId.value() == app.pwaAppId)
                {
                    isMatch = true;
                }
            }
        }

        if (isMatch)
        {
            int currentDistance = calculateDistance(window);
            if (!foundMatch || currentDistance < nearestWindowWithDistance.distance)
            {
                foundMatch = true;
                nearestWindowWithDistance.distance = currentDistance;
                nearestWindowWithDistance.window = window;
            }
        }
    }

    if (foundMatch)
    {
        return nearestWindowWithDistance;
    }

    return std::nullopt;
}

bool WorkspacesService::MoveWindowWithStateHandling(HWND window, const WorkspacesData::WorkspacesProject::Application& app)
{
    Logger::info(L"Moving window for app {} with advanced state handling", app.name);

    // Helper to convert screen coordinates to work area coordinates
    auto screenToWorkAreaCoords = [](HWND /*window*/, HMONITOR monitor, RECT& rect) {
        MONITORINFOEXW monitorInfo{ sizeof(MONITORINFOEXW) };
        GetMonitorInfoW(monitor, &monitorInfo);

        auto xOffset = monitorInfo.rcWork.left - monitorInfo.rcMonitor.left;
        auto yOffset = monitorInfo.rcWork.top - monitorInfo.rcMonitor.top;

        rect.left -= xOffset;
        rect.right -= xOffset;
        rect.top -= yOffset;
        rect.bottom -= yOffset;
    };

    // Get target monitor
    RECT targetRect = { app.position.x, app.position.y, app.position.x + app.position.width, app.position.y + app.position.height };
    HMONITOR targetMonitor = MonitorFromRect(&targetRect, MONITOR_DEFAULTTOPRIMARY);

    if (app.isMinimized)
    {
        // Use ShowWindow with SW_FORCEMINIMIZE to avoid animation
        if (!ShowWindow(window, SW_FORCEMINIMIZE))
        {
            Logger::error(L"ShowWindow minimize failed, {}", get_last_error_or_default(GetLastError()));
            return false;
        }
        return true;
    }

    // For normal/maximized windows, use SetWindowPos which is faster
    if (!app.isMaximized)
    {
        screenToWorkAreaCoords(window, targetMonitor, targetRect);

        // First ensure window is visible but not activated
        ShowWindow(window, SW_SHOWNOACTIVATE);

        // Use SetWindowPos with flags to disable animations and avoid activation
        auto result = ::SetWindowPos(window, nullptr, targetRect.left, targetRect.top, targetRect.right - targetRect.left, targetRect.bottom - targetRect.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_DEFERERASE);

        if (!result)
        {
            Logger::error(L"SetWindowPos failed, {}", get_last_error_or_default(GetLastError()));
            return false;
        }
    }
    else
    {
        // For maximized windows, first move to correct monitor, then maximize
        screenToWorkAreaCoords(window, targetMonitor, targetRect);

        // First ensure window is visible but not activated
        ShowWindow(window, SW_SHOWNOACTIVATE);

        // Move to correct position first
        ::SetWindowPos(window, nullptr, targetRect.left, targetRect.top, targetRect.right - targetRect.left, targetRect.bottom - targetRect.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_DEFERERASE);

        // Then maximize
        if (!ShowWindow(window, SW_MAXIMIZE))
        {
            Logger::error(L"ShowWindow maximize failed, {}", get_last_error_or_default(GetLastError()));
            return false;
        }
    }

    return true;
}

void WorkspacesService::MinimizeUnmanagedWindowsParallel(const std::vector<HWND>& movedWindows)
{
    auto currentWindows = EnumerateCurrentWindows();
    
    // Get current workspace for app protection
    auto currentWorkspace = m_launchingStatus ? m_launchingStatus->GetWorkspace() : WorkspacesData::WorkspacesProject{};
    
    if (currentWorkspace.apps.empty())
    {
        Logger::warn(L"No workspace apps available for protection in parallel minimization");
    }

    // Pre-filter windows that need protection (sequential for thread safety)
    std::vector<HWND> unmanagedWindows;
    Utils::PwaHelper pwaHelper;
    int protectedCount = 0;

    for (HWND window : currentWindows)
    {
        // Skip already moved windows
        if (std::find(movedWindows.begin(), movedWindows.end(), window) != movedWindows.end())
        {
            continue;
        }

        // Skip system popups
        if (WindowFilter::FilterPopup(window))
        {
            continue;
        }

        // **CRITICAL: Check if window belongs to workspace apps before minimizing**
        if (IsWindowInAppList(window, currentWorkspace, pwaHelper))
        {
            protectedCount++;
            continue; // Don't minimize workspace app windows
        }

        // Add to unmanaged list for minimization
        unmanagedWindows.push_back(window);
    }

    Logger::info(L"Found {} unmanaged windows to minimize, {} protected as workspace apps", unmanagedWindows.size(), protectedCount);

    // Use parallel processing for better performance with many windows
    std::vector<std::thread> workerThreads;
    std::atomic<int> processedCount{ 0 };
    const size_t numThreads = std::min(static_cast<size_t>(4), unmanagedWindows.size()); // Max 4 threads
    
    if (numThreads == 0 || unmanagedWindows.empty())
    {
        Logger::info(L"No unmanaged windows to minimize");
        return;
    }

    const size_t windowsPerThread = unmanagedWindows.size() / numThreads;

    for (size_t threadId = 0; threadId < numThreads; ++threadId)
    {
        size_t startIdx = threadId * windowsPerThread;
        size_t endIdx = (threadId == numThreads - 1) ? unmanagedWindows.size() : (threadId + 1) * windowsPerThread;

        workerThreads.emplace_back([&, startIdx, endIdx]() {
            int localMinimized = 0;
            for (size_t i = startIdx; i < endIdx; ++i)
            {
                HWND window = unmanagedWindows[i];

                // Minimize window
                if (ShowWindow(window, SW_FORCEMINIMIZE))
                {
                    localMinimized++;
                }
            }
            processedCount += localMinimized;
        });
    }

    // Wait for all threads to complete
    for (auto& thread : workerThreads)
    {
        thread.join();
    }

    Logger::info(L"Parallel minimization completed: {} minimized, {} protected as workspace apps", processedCount.load(), protectedCount);
}

bool WorkspacesService::ShouldContinueWaiting(const WorkspacesData::WorkspacesProject::Application& app,
                                              long maxWaitTimeMs,
                                              long currentWaitTimeMs)
{
    // Check if maximum wait time exceeded
    if (currentWaitTimeMs >= maxWaitTimeMs)
    {
        return false;
    }

    // Check if all instances of this app are launched and moved
    if (m_launchingStatus->AllInstancesOfTheAppLaunchedAndMoved(app))
    {
        return false;
    }

    // Check if all apps are done (early termination)
    if (m_launchingStatus->AllLaunchedAndMoved())
    {
        return false;
    }

    return true;
}

// Note: LaunchPackagedApp function is already defined in the anonymous namespace above