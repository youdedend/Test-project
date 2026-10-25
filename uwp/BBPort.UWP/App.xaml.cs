// SPDX-License-Identifier: GPL-2.0-or-later
// UWP Application shell configured for Xbox Series X|S / Xbox One 10-foot TV navigation:
// - Disables virtual mouse cursor (RequiresPointerMode = WhenRequested) so D-Pad/Left-Stick XY focus works natively.
// - Expands layout to full CoreWindow bounds (eliminates Xbox TV 5% overscan gutter).
// - Hooks PLM Suspending/Resuming to flush atomic saves and trim D3D12/D3D11 memory.

using System;
using System.Runtime.InteropServices;
using Windows.ApplicationModel;
using Windows.ApplicationModel.Activation;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace BBPort.UWP
{
    sealed partial class App : Application
    {
        [DllImport("BBPortNative.dll", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern void bbgpu_uwp_on_suspending();

        [DllImport("BBPortNative.dll", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern void bbgpu_uwp_on_resuming();

        public App()
        {
            this.InitializeComponent();
            // Essential on Xbox: disables the emulated free-floating mouse cursor so D-Pad & Left Stick
            // drive native XAML XY focus navigation across LauncherPage controls.
            this.RequiresPointerMode = ApplicationRequiresPointerMode.WhenRequested;
            this.Suspending += OnSuspending;
            this.Resuming += OnResuming;
        }

        protected override void OnLaunched(LaunchActivatedEventArgs e)
        {
            ApplicationView.GetForCurrentView().SetDesiredBoundsMode(ApplicationViewBoundsMode.UseCoreWindow);

            Frame rootFrame = Window.Current.Content as Frame;
            if (rootFrame == null)
            {
                rootFrame = new Frame();
                Window.Current.Content = rootFrame;
            }

            if (!e.PrelaunchActivated && rootFrame.Content == null)
            {
                rootFrame.Navigate(typeof(LauncherPage), e.Arguments);
            }

            Window.Current.Activate();
        }

        private void OnSuspending(object sender, SuspendingEventArgs e)
        {
            SuspendingDeferral deferral = e.SuspendingOperation.GetDeferral();
            try
            {
                bbgpu_uwp_on_suspending();
            }
            catch (DllNotFoundException)
            {
                // Native swapchain bridge not yet loaded while idling on XAML LauncherPage.
            }
            finally
            {
                deferral.Complete();
            }
        }

        private void OnResuming(object sender, object e)
        {
            try
            {
                bbgpu_uwp_on_resuming();
            }
            catch (DllNotFoundException)
            {
            }
        }
    }
}
