#pragma once

#include <memory>
#include <mutex>
#include <atomic>
#include <vector>
#include <thread>
#include <chrono>
#include <optional>

#include "IPCHelper.h"
#include "WorkspacesData.h"
#include "LaunchingStatus.h"
#include "LaunchingStateEnum.h"

// Forward declarations
namespace Utils
{
    class PwaHelper;
    namespace Apps
    {
        struct AppData;
        using AppList = std::vector<AppData>;
    }
}

// Error list type for launch errors
using ErrorList = std::vector<std::pair<std::wstring, std::wstring>>;

// Distance-based window matching
struct WindowWithDistance
{
    HWND window{};
    int distance{};
};

class WorkspacesService
{
public:
    WorkspacesService();
    ~WorkspacesService();

    void Start();

    void Stop();

private:
    // Service state
    std::atomic<bool> m_enabled{ false };
    std::atomic<bool> m_shouldStop{ false };
    std::atomic<bool> m_processing{ false }; // Single request processing flag
    std::mutex m_serviceMutex;

    // Integrated launcher/arranger state
    std::unique_ptr<LaunchingStatus> m_launchingStatus;
    std::vector<HWND> m_windowsBefore; // Windows captured before processing
    std::vector<WorkspacesData::WorkspacesProject::Monitor> m_monitors;
    ErrorList m_launchErrors;
    mutable std::mutex m_launchErrorsMutex;
    
    // Cached app list for performance optimization
    Utils::Apps::AppList m_cachedAppsList;
    std::chrono::steady_clock::time_point m_appsCacheTime;
    static constexpr std::chrono::hours APPS_CACHE_DURATION{24 * 365}; // Cache for 1 year (effectively permanent)

    // IPC communication
    std::unique_ptr<IPCHelper> m_ipcHelper;

    /**
     * IPC message processing callback
     * @param message Received workspace ID
     */
    void OnIPCMessage(const std::wstring& message);

    /**
     * Process workspace launch request (main entry point)
     * @param workspaceId Workspace ID
     */
    void ProcessWorkspace(const std::wstring& workspaceId);

    /**
     * Execute the 4-step workspace processing workflow
     * @param workspace Workspace data
     */
    void ExecuteWorkspaceSequence(const WorkspacesData::WorkspacesProject& workspace);

    /**
     * Step 1: Process existing windows
     * - Detect current system windows
     * - Match workspace applications
     * - Move matching windows to target positions
     * @param workspace Workspace data
     * @return List of moved window handles
     */
    std::vector<HWND> ProcessExistingWindows(const WorkspacesData::WorkspacesProject& workspace);

    /**
     * Step 2: Launch missing applications
     * - Check if workspace applications already have windows
     * - Launch missing applications
     * @param workspace Workspace data
     * @param existingWindows Existing windows list
     */
    void LaunchMissingApplications(const WorkspacesData::WorkspacesProject& workspace, 
                                   const std::vector<HWND>& existingWindows);

    /**
     * Step 2 Advanced: Launch missing applications with enhanced logic
     * - Advanced sequencing and waiting logic
     * - Better error handling and state management
     * @param workspace Workspace data
     * @param existingWindows Existing windows list
     */
    void LaunchMissingApplicationsAdvanced(const WorkspacesData::WorkspacesProject& workspace,
                                          const std::vector<HWND>& existingWindows);

    /**
     * Step 3: Process newly launched windows
     * - Monitor for newly appearing windows
     * - Match workspace applications and move to positions
     * @param workspace Workspace data
     * @param movedWindows List of moved windows (will be updated)
     */
    void ProcessNewWindows(const WorkspacesData::WorkspacesProject& workspace,
                          std::vector<HWND>& movedWindows);

    /**
     * Step 3 Advanced: Process newly launched windows with enhanced logic
     * - Better timing and matching algorithms
     * - State-aware window processing
     * @param workspace Workspace data
     * @param movedWindows List of moved windows (will be updated)
     */
    void ProcessNewWindowsAdvanced(const WorkspacesData::WorkspacesProject& workspace,
                                  std::vector<HWND>& movedWindows);

    /**
     * Step 4: Minimize non-target windows
     * - Minimize other windows that don't belong to the workspace
     * @param movedWindows List of moved windows (should not be minimized)
     */
    void MinimizeUnmanagedWindows(const std::vector<HWND>& movedWindows);

    // Helper methods

    /**
     * Check if window matches application
     * @param window Window handle
     * @param app Application data
     * @param pwaHelper PWA helper tool
     * @return Whether it matches
     */
    bool IsWindowMatchApp(HWND window, const WorkspacesData::WorkspacesProject::Application& app, 
                         Utils::PwaHelper& pwaHelper);

    /**
     * Move window to specified position
     * @param window Window handle
     * @param app Application data (contains position info)
     * @return Whether successful
     */
    bool MoveWindowToPosition(HWND window, const WorkspacesData::WorkspacesProject::Application& app);

    /**
     * Check if window belongs to the current workspace applications
     * - Prevents workspace app windows from being minimized
     * - Uses AUMID, path, and process name matching
     * @param window Window handle to check
     * @param workspace Workspace data containing app list
     * @param pwaHelper PWA helper tool
     * @return true if window belongs to workspace (don't minimize), false otherwise
     */
    bool IsWindowInAppList(HWND window, const WorkspacesData::WorkspacesProject& workspace, Utils::PwaHelper& pwaHelper);

    /**
     * Load workspace data
     * @param workspaceId Workspace ID
     * @param workspace Output workspace data
     */
    void LoadWorkspace(const std::wstring& workspaceId, WorkspacesData::WorkspacesProject& workspace);

    /**
     * Enumerate current system windows
     * @return Window handles list
     */
    std::vector<HWND> EnumerateCurrentWindows();

    /**
     * Launch application
     * @param app Application data
     * @return Whether launch was successful
     */
    bool LaunchApplication(const WorkspacesData::WorkspacesProject::Application& app);

    // Advanced Launcher/Arranger integration methods

    /**
     * Get or refresh cached app list for performance optimization
     * @return Reference to cached app list
     */
    const Utils::Apps::AppList& GetCachedAppsList();

    /**
     * Enhanced app launching logic from AppLauncher
     * @param app Application data
     * @param launchErrors Error accumulator
     * @return Whether launch was successful
     */
    bool LaunchAppWithFullLogic(const WorkspacesData::WorkspacesProject::Application& app, 
                               ErrorList& launchErrors);

    /**
     * Advanced window matching logic from WindowArranger
     * @param app Application data
     * @param movedWindows List of already moved windows
     * @param pwaHelper PWA helper
     * @return Best matching window with distance, if found
     */
    std::optional<WindowWithDistance> GetNearestWindow(
        const WorkspacesData::WorkspacesProject::Application& app,
        const std::vector<HWND>& movedWindows, 
        Utils::PwaHelper& pwaHelper);

    /**
     * Advanced window movement logic with proper state handling
     * @param window Window to move
     * @param app Application data
     * @return Whether movement was successful
     */
    bool MoveWindowWithStateHandling(HWND window, const WorkspacesData::WorkspacesProject::Application& app);

    /**
     * Parallel window minimization for unmanaged windows
     * @param movedWindows Windows that should not be minimized
     */
    void MinimizeUnmanagedWindowsParallel(const std::vector<HWND>& movedWindows);

    /**
     * Check if application instance should continue waiting
     * @param app Application to check
     * @param maxWaitTimeMs Maximum wait time
     * @param currentWaitTimeMs Current elapsed wait time
     * @return Whether to continue waiting
     */
    bool ShouldContinueWaiting(const WorkspacesData::WorkspacesProject::Application& app,
                              long maxWaitTimeMs, long currentWaitTimeMs);
};