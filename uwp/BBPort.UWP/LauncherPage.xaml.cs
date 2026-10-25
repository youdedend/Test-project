// SPDX-License-Identifier: GPL-2.0-or-later
// UWP XAML LauncherPage replacing desktop Tkinter launcher/bbport_launcher_win.py:
// - Zero FileOpenPicker / FolderPicker dialogs (avoids Xbox Dev Mode Shell picker hangs/WACK issues).
// - Deterministic First-Start Check against ApplicationData.Current.LocalFolder\game\CUSA03173
//   (parsing binary sce_sys\param.sfo and inspecting eboot.bin / boot-linked.bin headers).
// - Live Xbox hardware & Game Mode 5 GiB resource budget rating via AnalyticsInfo + MemoryManager.
// - Live Windows.Gaming.Input.Gamepad hotplug status without desktop DirectInput or SDL3.dll.

using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using Windows.Gaming.Input;
using Windows.Storage;
using Windows.System;
using Windows.System.Profile;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace BBPort.UWP
{
    public sealed partial class LauncherPage : Page
    {
        private bool _uiReady = false;
        private bool _assetsReady = false;

        [DllImport("BBPortNative.dll", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int LaunchBloodborneFromXaml(IntPtr coreWindowAbi, uint width, uint height);

        public LauncherPage()
        {
            this.InitializeComponent();
            this.Loaded += OnPageLoaded;
            this.Unloaded += OnPageUnloaded;
        }

        private async void OnPageLoaded(object sender, RoutedEventArgs e)
        {
            LoadPersistedSettings();
            _uiReady = true;

            Gamepad.GamepadAdded += OnGamepadChanged;
            Gamepad.GamepadRemoved += OnGamepadChanged;
            UpdateGamepadBadge();
            EvaluateHardwareRating();
            await RunFirstStartAssetAuditAsync();

            LaunchButton.Focus(FocusState.Programmatic);
        }

        private void OnPageUnloaded(object sender, RoutedEventArgs e)
        {
            Gamepad.GamepadAdded -= OnGamepadChanged;
            Gamepad.GamepadRemoved -= OnGamepadChanged;
        }

        private async void OnGamepadChanged(object sender, Gamepad e)
        {
            await Dispatcher.RunAsync(CoreDispatcherPriority.Normal, UpdateGamepadBadge);
        }

        private void UpdateGamepadBadge()
        {
            int count = Gamepad.Gamepads.Count;
            ControllerBadgeText.Text = count > 0
                ? $"Gamepad: {count} Connected (WGI Ready)"
                : "Gamepad: None (Press Xbox Button)";
        }

        private void EvaluateHardwareRating()
        {
            string family = AnalyticsInfo.VersionInfo.DeviceFamily;
            ulong limitBytes = MemoryManager.AppMemoryUsageLimit;
            ulong limitMb = limitBytes / (1024UL * 1024UL);

            DeviceFamilyText.Text = $"Device Family: {family} (x64)";
            MemoryCapText.Text = $"AppMemoryUsageLimit: {limitMb:N0} MiB (DirectMemory capped to 3,328 MiB)";

            bool isXbox = string.Equals(family, "Windows.Xbox", StringComparison.OrdinalIgnoreCase);
            if (isXbox && limitMb < 2048)
            {
                ResourceModeVerdictText.Text =
                    "WARNING: Package is running in Xbox APP Resource Mode (~1 GiB cap, D3D12 disabled). " +
                    "Open Dev Home ->BBPort.UWP -> View details -> Change App Type to 'Game' to unlock 5 GiB + D3D12 FL 12_0.";
            }
            else if (isXbox)
            {
                ResourceModeVerdictText.Text =
                    "PASS: Xbox GAME Resource Mode active (~5,120 MiB cap, Direct3D 12 Feature Level 12_0 enabled).";
            }
            else
            {
                ResourceModeVerdictText.Text =
                    "Desktop UWP Sandbox active (Direct3D 12 Feature Level 12_0 + AppContainer *FromApp guardrails).";
            }
        }

        private async Task RunFirstStartAssetAuditAsync()
        {
            StorageFolder local = ApplicationData.Current.LocalFolder;
            string gameDir = Path.Combine(local.Path, "game", "CUSA03173");
            if (!File.Exists(Path.Combine(gameDir, "sce_sys", "param.sfo")) &&
                File.Exists(@"D:\DevelopmentFiles\Bloodborne\CUSA03173\sce_sys\param.sfo"))
            {
                gameDir = @"D:\DevelopmentFiles\Bloodborne\CUSA03173";
            }

            ProvisionPathText.Text = $"Resolved /app0 Path: {gameDir}";

            bool sfoOk = false;
            bool exeOk = false;
            bool modsOk = false;

            await Task.Run(() =>
            {
                string sfoPath = Path.Combine(gameDir, "sce_sys", "param.sfo");
                string titleId = "";
                string appVer = "";
                if (File.Exists(sfoPath) && TryParseParamSfo(File.ReadAllBytes(sfoPath), out titleId, out appVer))
                {
                    sfoOk = titleId.StartsWith("CUSA", StringComparison.Ordinal);
                    _ = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        SfoStatusText.Text = $"• sce_sys\\param.sfo: PASS ({titleId}, APP_VER {appVer})";
                    });
                }
                else
                {
                    _ = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        SfoStatusText.Text = "• sce_sys\\param.sfo: MISSING (Run uwp\\provision.ps1)";
                    });
                }

                string bootLinked = Path.Combine(gameDir, "boot-linked.bin");
                string ebootPath = Path.Combine(gameDir, "eboot.bin");
                string exeDiag = "MISSING";
                if (File.Exists(bootLinked) && CheckHeaderMagic(bootLinked, out exeDiag))
                {
                    exeOk = true;
                }
                else if (File.Exists(ebootPath) && CheckHeaderMagic(ebootPath, out exeDiag))
                {
                    exeOk = true;
                }

                _ = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
                {
                    ExecutableStatusText.Text = $"• Executable Image: {exeDiag}";
                });

                bool hasLibc = File.Exists(Path.Combine(gameDir, "sce_module", "libc.prx"));
                bool hasFios = File.Exists(Path.Combine(gameDir, "sce_module", "libSceFios2.prx"));
                bool hasDvd = Directory.Exists(Path.Combine(gameDir, "dvdroot_ps4"));
                modsOk = hasDvd && (File.Exists(bootLinked) || (hasLibc && hasFios));

                _ = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
                {
                    ModulesStatusText.Text =
                        $"• libc.prx={(hasLibc ? "OK" : "NO")}, libSceFios2.prx={(hasFios ? "OK" : "NO")}, dvdroot_ps4={(hasDvd ? "OK" : "NO")}";
                });
            });

            _assetsReady = sfoOk && exeOk && modsOk;
            LaunchButton.IsEnabled = _assetsReady;
            StatusFooterText.Text = _assetsReady
                ? "All CUSA03173 v1.09 checks passed. Press A on 'BEGIN THE HUNT' to enter Yharnam."
                : "Assets incomplete in LocalState\\game\\CUSA03173. Provision via USB NTFS or SFTP and press Re-Scan.";
        }

        private static bool TryParseParamSfo(byte[] data, out string titleId, out string appVer)
        {
            titleId = "";
            appVer = "";
            if (data == null || data.Length < 20) return false;
            if (data[0] != 0x00 || data[1] != (byte)'P' || data[2] != (byte)'S' || data[3] != (byte)'F')
                return false;

            uint keyOff = BitConverter.ToUInt32(data, 8);
            uint valOff = BitConverter.ToUInt32(data, 12);
            uint count = BitConverter.ToUInt32(data, 16);
            if (count > 512 || 20 + count * 16 > data.Length) return false;

            for (uint i = 0; i < count; i++)
            {
                int entry = (int)(20 + i * 16);
                ushort kRel = BitConverter.ToUInt16(data, entry);
                ushort fmt = BitConverter.ToUInt16(data, entry + 2);
                uint vLen = BitConverter.ToUInt32(data, entry + 4);
                uint vRel = BitConverter.ToUInt32(data, entry + 12);

                int kStart = (int)(keyOff + kRel);
                int kEnd = kStart;
                while (kEnd < data.Length && data[kEnd] != 0) kEnd++;
                if (kStart >= data.Length) return false;
                string key = Encoding.UTF8.GetString(data, kStart, kEnd - kStart);

                if ((fmt == 0x0204 || fmt == 0x0004) && valOff + vRel + vLen <= data.Length)
                {
                    string val = Encoding.UTF8.GetString(data, (int)(valOff + vRel), (int)vLen).TrimEnd('\0');
                    if (key == "TITLE_ID") titleId = val;
                    else if (key == "APP_VER") appVer = val;
                }
            }
            return !string.IsNullOrEmpty(titleId);
        }

        private static bool CheckHeaderMagic(string path, out string diag)
        {
            byte[] hdr = new byte[64];
            using (FileStream fs = File.OpenRead(path))
            {
                int n = fs.Read(hdr, 0, hdr.Length);
                if (n < 16)
                {
                    diag = "INVALID (Truncated)";
                    return false;
                }
            }
            if (hdr[0] == (byte)'B' && hdr[1] == (byte)'B' && hdr[2] == (byte)'P' && hdr[3] == (byte)'R')
            {
                diag = "PASS (Pre-linked BBPROBE boot image)";
                return true;
            }
            if (hdr[0] == 0x7F && hdr[1] == (byte)'E' && hdr[2] == (byte)'L' && hdr[3] == (byte)'F' && hdr[4] == 2)
            {
                diag = "PASS (Decrypted PS4 x86-64 ELF64)";
                return true;
            }
            if (hdr[0] == 0x4F && hdr[1] == 0x15 && hdr[2] == 0x3D && hdr[3] == 0x1D)
            {
                diag = "FAIL (Encrypted PS4 SELF — decrypt eboot.bin on PS4 before provisioning)";
                return false;
            }
            diag = "FAIL (Unrecognized binary header)";
            return false;
        }

        private void LoadPersistedSettings()
        {
            var values = ApplicationData.Current.LocalSettings.Values;
            SelectByTag(FpsCombo, values.ContainsKey("target_fps") ? values["target_fps"].ToString() : "60");
            SelectByTag(OutputHeightCombo, values.ContainsKey("output_height") ? values["output_height"].ToString() : "1080");
            SelectByTag(PresetCombo, values.ContainsKey("render_preset") ? values["render_preset"].ToString() : "1");
            if (values.ContainsKey("enable_sound_hack")) SoundHackToggle.IsOn = (bool)values["enable_sound_hack"];
            if (values.ContainsKey("swap_confirm_circle")) SwapConfirmToggle.IsOn = (bool)values["swap_confirm_circle"];
            if (values.ContainsKey("enable_online_shadnet")) OnlineToggle.IsOn = (bool)values["enable_online_shadnet"];
        }

        private static void SelectByTag(ComboBox box, string tag)
        {
            for (int i = 0; i < box.Items.Count; i++)
            {
                if (box.Items[i] is ComboBoxItem item && string.Equals(item.Tag?.ToString(), tag, StringComparison.Ordinal))
                {
                    box.SelectedIndex = i;
                    return;
                }
            }
        }

        private void PersistCurrentSettings()
        {
            if (!_uiReady) return;
            var values = ApplicationData.Current.LocalSettings.Values;
            if (FpsCombo.SelectedItem is ComboBoxItem fpsItem)
                values["target_fps"] = int.Parse(fpsItem.Tag.ToString());
            if (OutputHeightCombo.SelectedItem is ComboBoxItem hItem)
                values["output_height"] = int.Parse(hItem.Tag.ToString());
            if (PresetCombo.SelectedItem is ComboBoxItem pItem)
                values["render_preset"] = int.Parse(pItem.Tag.ToString());
            values["enable_sound_hack"] = SoundHackToggle.IsOn;
            values["swap_confirm_circle"] = SwapConfirmToggle.IsOn;
            values["enable_online_shadnet"] = OnlineToggle.IsOn;
            values["shader_compile_threads"] = 2u;
            values["dmem_budget_mb"] = 3328u;
        }

        private void OnSettingChanged(object sender, SelectionChangedEventArgs e) => PersistCurrentSettings();
        private void OnSettingToggled(object sender, RoutedEventArgs e) => PersistCurrentSettings();

        private async void OnRescanClicked(object sender, RoutedEventArgs e)
        {
            EvaluateHardwareRating();
            await RunFirstStartAssetAuditAsync();
        }

        private void OnLaunchClicked(object sender, RoutedEventArgs e)
        {
            PersistCurrentSettings();
            if (!_assetsReady) return;

            CoreWindow cw = CoreWindow.GetForCurrentThread();
            IntPtr cwAbi = Marshal.GetIUnknownForObject(cw);
            try
            {
                uint w = (uint)Math.Max(1280, (int)cw.Bounds.Width);
                uint h = (uint)Math.Max(720, (int)cw.Bounds.Height);
                LaunchBloodborneFromXaml(cwAbi, w, h);
            }
            finally
            {
                if (cwAbi != IntPtr.Zero) Marshal.Release(cwAbi);
            }
        }
    }
}
