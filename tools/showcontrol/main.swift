// Show Control by Sentia: the home's lighting desk and show control. It listens to Spotify on this
// Mac and drives everything visual from it: the LMD-9020 display (its visualizer), Govee lamps
// segment by segment, Matter lights. Kicks go out the moment they're heard; every output is held
// back just enough to land with the sound from the speakers. Build with build.sh.
//
// macOS asks once to allow screen and system audio recording (that's how one app's sound is
// captured; nothing of the screen is used), access to the local network, and control of Spotify.

import AppKit
import SwiftUI

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    private var window: NSWindow!
    private var show: Show!
    private var activity: NSObjectProtocol?
    private var web: WebServer?

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Keep running at full rate with the window closed: no App Nap.
        activity = ProcessInfo.processInfo.beginActivity(options: [.userInitiatedAllowingIdleSystemSleep, .latencyCritical],
                                                         reason: "Running the show")
        Sentia.registerFonts()
        show = Show()
        web = WebServer(show: show)
        let menu = NSMenu(), appItem = NSMenuItem()
        menu.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "Quit Show Control", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        NSApp.mainMenu = menu

        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1180, height: 760),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        window.title = "Show Control"
        window.titleVisibility = .hidden
        window.titlebarAppearsTransparent = true
        window.isReleasedWhenClosed = false
        window.backgroundColor = NSColor(Sentia.ink)
        window.appearance = NSAppearance(named: .darkAqua)
        window.minSize = NSSize(width: 1080, height: 700)
        window.contentView = NSHostingView(rootView: RootView(show: show))
        window.delegate = self
        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.show.windowVisible = self.window.isVisible && self.window.occlusionState.contains(.visible)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }
    func applicationWillTerminate(_ notification: Notification) { show.shutDown() }  // the lights as they were
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        window.makeKeyAndOrderFront(nil)
        return true
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
