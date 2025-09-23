#include "pch.h"

#include <workspaces-common/IPCHelper.h>
#include <common/logger/logger.h>

#include <iostream>
#include <string>

namespace
{
    const std::wstring SERVICE_PIPE_NAME = L"PowerToys_WorkspacesService";
}

int wmain(int argc, wchar_t* argv[])
{
    Logger::init(L"WorkspacesLauncherClient");
    
    if (argc < 2)
    {
        Logger::error(L"Usage: WorkspacesLauncherClient.exe <workspace-id>");
        std::wcout << L"Usage: WorkspacesLauncherClient.exe <workspace-id>" << std::endl;
        return 1;
    }
    
    std::wstring workspaceId = argv[1];
    Logger::info(L"Launching workspace: {}", workspaceId);
    
    try
    {
        // 创建 IPC 客户端向服务发送请求
        IPCHelper ipcClient(
            L"", // 不需要接收管道
            SERVICE_PIPE_NAME, // 发送管道
            nullptr // 不需要消息处理回调
        );
        
        // 发送工作区 ID 给服务
        if (ipcClient.send_message(workspaceId))
        {
            Logger::info(L"Successfully sent workspace launch request: {}", workspaceId);
            std::wcout << L"Workspace launch request sent: " << workspaceId << std::endl;
            return 0;
        }
        else
        {
            Logger::error(L"Failed to send workspace launch request: {}", workspaceId);
            std::wcout << L"Failed to send workspace launch request: " << workspaceId << std::endl;
            return 1;
        }
    }
    catch (const std::exception& e)
    {
        Logger::error(L"Exception: {}", e.what());
        std::wcout << L"Error: " << e.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        Logger::error(L"Unknown exception occurred");
        std::wcout << L"Unknown error occurred" << std::endl;
        return 1;
    }
}