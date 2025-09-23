// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using Microsoft.CmdPal.Services.Profiles;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Microsoft.CmdPal.UI.Settings;

public sealed partial class ProfilesPage : Page
{
    private ProfileManager? _profileManager;

    public ObservableCollection<ProfileViewModel> Profiles { get; } = new();

    public ObservableCollection<ActionViewModel> Actions { get; } = new();

    public ProfilesPage()
    {
        this.InitializeComponent();
        this.Loaded += OnLoaded;
    }

    private async void OnLoaded(object sender, RoutedEventArgs e)
    {
        // TODO: Get ProfileManager from DI container or service locator
        // For now, we'll create a new instance
        _profileManager = new ProfileManager();
        await LoadProfilesAsync();
        await LoadActionsAsync();
        UpdateUI();
    }

    private async Task LoadProfilesAsync()
    {
        if (_profileManager == null) return;

        Profiles.Clear();
        var profiles = await _profileManager.GetAllProfilesAsync();
        var activeProfile = await _profileManager.GetActiveProfileAsync();

        foreach (var profile in profiles)
        {
            var viewModel = new ProfileViewModel
            {
                Name = profile.Key,
                IsActive = profile.Key == activeProfile?.Name,
                ActionCount = profile.Value.Actions?.Count ?? 0
            };
            Profiles.Add(viewModel);
        }

        ProfileListView.ItemsSource = Profiles;
    }

    private async Task LoadActionsAsync()
    {
        // TODO: Get available actions from action providers
        // For now, create sample actions
        Actions.Clear();

        var sampleActions = new[]
        {
            new ActionViewModel
            {
                ActionId = "microsoft.advancedpaste+paste",
                DisplayName = "Advanced Paste",
                ProviderId = "microsoft.advancedpaste",
                Description = "Enhanced paste functionality with formatting options",
                IsEnabled = false
            },
            new ActionViewModel
            {
                ActionId = "microsoft.colorpicker+pick",
                DisplayName = "Color Picker",
                ProviderId = "microsoft.colorpicker",
                Description = "Pick colors from anywhere on screen",
                IsEnabled = false
            },
            new ActionViewModel
            {
                ActionId = "microsoft.powerrename+rename",
                DisplayName = "PowerRename",
                ProviderId = "microsoft.powerrename",
                Description = "Bulk rename files with advanced patterns",
                IsEnabled = false
            }
        };

        foreach (var action in sampleActions)
        {
            Actions.Add(action);
        }

        ActionConfigurationList.ItemsSource = Actions;
    }

    private async void OnAddProfile(object sender, RoutedEventArgs e)
    {
        var dialog = new ContentDialog
        {
            Title = "Add New Profile",
            PrimaryButtonText = "Add",
            CloseButtonText = "Cancel",
            XamlRoot = this.XamlRoot
        };

        var textBox = new TextBox
        {
            PlaceholderText = "Enter profile name...",
            Margin = new Thickness(0, 12, 0, 0)
        };

        var stackPanel = new StackPanel();
        stackPanel.Children.Add(new TextBlock { Text = "Profile Name:" });
        stackPanel.Children.Add(textBox);
        dialog.Content = stackPanel;

        var result = await dialog.ShowAsync();
        if (result == ContentDialogResult.Primary && !string.IsNullOrWhiteSpace(textBox.Text))
        {
            if (_profileManager != null)
            {
                await _profileManager.CreateProfileAsync(textBox.Text.Trim());
                await LoadProfilesAsync();
                UpdateUI();
            }
        }
    }

    private async void OnRemoveProfile(object sender, RoutedEventArgs e)
    {
        var selectedProfile = ProfileListView.SelectedItem as ProfileViewModel;
        if (selectedProfile == null) return;

        var dialog = new ContentDialog
        {
            Title = "Remove Profile",
            Content = $"Are you sure you want to remove '{selectedProfile.Name}'?",
            PrimaryButtonText = "Remove",
            CloseButtonText = "Cancel",
            XamlRoot = this.XamlRoot
        };

        var result = await dialog.ShowAsync();
        if (result == ContentDialogResult.Primary && _profileManager != null)
        {
            await _profileManager.DeleteProfileAsync(selectedProfile.Name);
            await LoadProfilesAsync();
            UpdateUI();
        }
    }

    private async void OnActivateProfile(object sender, RoutedEventArgs e)
    {
        var selectedProfile = ProfileListView.SelectedItem as ProfileViewModel;
        if (selectedProfile == null || _profileManager == null) return;

        await _profileManager.ActivateProfileAsync(selectedProfile.Name);
        await LoadProfilesAsync();
        UpdateUI();
    }

    private async void OnRenameProfile(object sender, RoutedEventArgs e)
    {
        var button = sender as Button;
        var profile = button?.Tag as ProfileViewModel;
        if (profile == null) return;

        var dialog = new ContentDialog
        {
            Title = "Rename Profile",
            PrimaryButtonText = "Rename",
            CloseButtonText = "Cancel",
            XamlRoot = this.XamlRoot
        };

        var textBox = new TextBox
        {
            Text = profile.Name,
            Margin = new Thickness(0, 12, 0, 0)
        };

        var stackPanel = new StackPanel();
        stackPanel.Children.Add(new TextBlock { Text = "New Profile Name:" });
        stackPanel.Children.Add(textBox);
        dialog.Content = stackPanel;

        var result = await dialog.ShowAsync();
        if (result == ContentDialogResult.Primary &&
            !string.IsNullOrWhiteSpace(textBox.Text) &&
            textBox.Text.Trim() != profile.Name &&
            _profileManager != null)
        {
            await _profileManager.RenameProfileAsync(profile.Name, textBox.Text.Trim());
            await LoadProfilesAsync();
            UpdateUI();
        }
    }

    private async void OnProfileSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        var selectedProfile = ProfileListView.SelectedItem as ProfileViewModel;

        RemoveProfileButton.IsEnabled = selectedProfile != null && !selectedProfile.IsActive;
        ActivateProfileButton.IsEnabled = selectedProfile != null && !selectedProfile.IsActive;
        ActionConfigurationExpander.IsEnabled = selectedProfile != null;

        if (selectedProfile != null && _profileManager != null)
        {
            await LoadActionsForProfileAsync(selectedProfile.Name);
        }

        UpdateUI();
    }

    private async Task LoadActionsForProfileAsync(string profileName)
    {
        if (_profileManager == null) return;

        var profile = await _profileManager.GetProfileAsync(profileName);
        if (profile != null)
        {
            // Update action enabled states based on profile configuration
            foreach (var action in Actions)
            {
                action.IsEnabled = profile.Actions?.GetValueOrDefault(action.ActionId, false) ?? false;
            }
        }
    }

    private async void OnActionToggled(object sender, RoutedEventArgs e)
    {
        var checkBox = sender as CheckBox;
        var action = checkBox?.Tag as ActionViewModel;
        var selectedProfile = ProfileListView.SelectedItem as ProfileViewModel;

        if (action == null || selectedProfile == null || _profileManager == null) return;

        var profile = await _profileManager.GetProfileAsync(selectedProfile.Name);
        if (profile != null)
        {
            profile.Actions ??= new Dictionary<string, bool>();
            profile.Actions[action.ActionId] = action.IsEnabled;

            await _profileManager.SaveProfileAsync(selectedProfile.Name, profile);
            await LoadProfilesAsync(); // Refresh to update action counts
            UpdateUI();
        }
    }

    private void UpdateUI()
    {
        var selectedProfile = ProfileListView.SelectedItem as ProfileViewModel;
        var activeProfile = Profiles.FirstOrDefault(p => p.IsActive);

        CurrentProfileText.Text = activeProfile != null
            ? $"Current Profile: {activeProfile.Name}"
            : "Current Profile: None";

        if (selectedProfile != null)
        {
            var enabledCount = Actions.Count(a => a.IsEnabled);
            ProfileActionsCountText.Text = $"Enabled Actions: {enabledCount}/{Actions.Count}";
        }
        else
        {
            ProfileActionsCountText.Text = "Enabled Actions: -";
        }
    }
}

public class ProfileViewModel : INotifyPropertyChanged
{
    private string _name = string.Empty;
    private bool _isActive;
    private int _actionCount;

    public string Name
    {
        get => _name;
        set
        {
            if (_name != value)
            {
                _name = value;
                OnPropertyChanged();
            }
        }
    }

    public bool IsActive
    {
        get => _isActive;
        set
        {
            if (_isActive != value)
            {
                _isActive = value;
                OnPropertyChanged();
            }
        }
    }

    public int ActionCount
    {
        get => _actionCount;
        set
        {
            if (_actionCount != value)
            {
                _actionCount = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(ActionCountText));
            }
        }
    }

    public string ActionCountText => $"{ActionCount} actions enabled";

    public event PropertyChangedEventHandler? PropertyChanged;

    protected virtual void OnPropertyChanged([CallerMemberName] string? propertyName = null)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}

public class ActionViewModel : INotifyPropertyChanged
{
    private bool _isEnabled;

    public required string ActionId { get; set; }
    public required string DisplayName { get; set; }
    public required string ProviderId { get; set; }
    public string Description { get; set; } = string.Empty;

    public bool IsEnabled
    {
        get => _isEnabled;
        set
        {
            if (_isEnabled != value)
            {
                _isEnabled = value;
                OnPropertyChanged();
            }
        }
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    protected virtual void OnPropertyChanged([CallerMemberName] string? propertyName = null)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}