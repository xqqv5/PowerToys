// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System.IO.Pipes;
using System.Text;

namespace PowerToys.WorkspacesMCP.Services
{
    /// <summary>
    /// Client for communicating with the native WorkspacesService via named pipes
    /// </summary>
    public class WorkspacesServiceClient : IDisposable
    {
        private const string PIPENAME = "powertoys_workspaces_service_";
        private const int CONNECTIONTIMEOUTMS = 5000;
        private readonly object _lock = new();
        private bool _disposed;

        /// <summary>
        /// Send a workspace launch request to the WorkspacesService
        /// </summary>
        /// <param name="workspaceId">The workspace ID to launch</param>
        /// <param name="cancellationToken">Cancellation token</param>
        /// <returns>True if the message was sent successfully</returns>
        public async Task<bool> LaunchWorkspaceAsync(string workspaceId, CancellationToken cancellationToken = default)
        {
            if (string.IsNullOrWhiteSpace(workspaceId))
            {
                throw new ArgumentException("Workspace ID cannot be null or empty", nameof(workspaceId));
            }

            try
            {
                using var client = new NamedPipeClientStream(".", PIPENAME, PipeDirection.InOut);

                // Connect to the WorkspacesService
                await client.ConnectAsync(CONNECTIONTIMEOUTMS, cancellationToken);

                if (!client.IsConnected)
                {
                    return false;
                }

                // Send the workspace ID as UTF-16 encoded message (compatible with std::wstring)
                var messageBytes = Encoding.Unicode.GetBytes(workspaceId);
                await client.WriteAsync(messageBytes, cancellationToken);
                await client.FlushAsync(cancellationToken);

                return true;
            }
            catch (TimeoutException)
            {
                // WorkspacesService might not be running
                return false;
            }
            catch (IOException)
            {
                // Pipe connection failed
                return false;
            }
            catch (UnauthorizedAccessException)
            {
                // Permission denied
                return false;
            }
        }

        /// <summary>
        /// Check if WorkspacesService is available (pipe exists and accessible)
        /// </summary>
        /// <returns>True if the service appears to be available</returns>
        public async Task<bool> IsServiceAvailableAsync(CancellationToken cancellationToken = default)
        {
            try
            {
                using var client = new NamedPipeClientStream(".", PIPENAME, PipeDirection.InOut);
                await client.ConnectAsync(1000, cancellationToken); // Short timeout for availability check
                return client.IsConnected;
            }
            catch
            {
                return false;
            }
        }

        public void Dispose()
        {
            Dispose(true);
            GC.SuppressFinalize(this);
        }

        protected virtual void Dispose(bool disposing)
        {
            if (!_disposed && disposing)
            {
                // Cleanup if needed
                _disposed = true;
            }
        }
    }
}
