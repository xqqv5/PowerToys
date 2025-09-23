#pragma once

#include <shared_mutex>

#include <WorkspacesLib/WorkspacesData.h>

class LaunchingStatus
{
public:
    LaunchingStatus(const WorkspacesData::WorkspacesProject& project);
    ~LaunchingStatus() = default;

    bool AllLaunched() noexcept;
    bool AllLaunchedAndMoved() noexcept;
    bool AllInstancesOfTheAppLaunchedAndMoved(const WorkspacesData::WorkspacesProject::Application& app) noexcept;

    const WorkspacesData::LaunchingAppStateMap& Get() noexcept;
    std::optional<WorkspacesData::LaunchingAppState> Get(const WorkspacesData::WorkspacesProject::Application& app) noexcept;
    std::optional<WorkspacesData::LaunchingAppState> GetNext(LaunchingState state) noexcept;
    
    /**
     * Get the workspace project data for app protection
     * @return Workspace project reference
     */
    const WorkspacesData::WorkspacesProject& GetWorkspace() const noexcept;
    
    bool IsWindowProcessed(HWND window) noexcept;

    void Update(const WorkspacesData::WorkspacesProject::Application& app, LaunchingState state);
    void Update(const WorkspacesData::WorkspacesProject::Application& app, HWND window, LaunchingState state);
    void Cancel();
    
private:
    const WorkspacesData::WorkspacesProject m_project; // Store workspace for app protection
    WorkspacesData::LaunchingAppStateMap m_appsState;
    std::shared_mutex m_mutex;
};
