// Show Control by Sentia: the home's lighting desk and show control. It listens to Spotify on this
// Mac and drives everything visual from it: the LMD-9020 display (its visualizer), Govee lamps
// segment by segment, Matter lights. Kicks go out the moment they're heard; every output is held
// back just enough to land with the sound from the speakers. Build with build.sh.
//
// macOS asks once to allow screen and system audio recording (that's how one app's sound is
// captured; nothing of the screen is used), access to the local network, and control of Spotify.

import AppKit
import Combine
import SwiftUI

// It lives in the menu bar; the Dock icon is there only while the window is open.
final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate {
    private var window: NSWindow!
    private var show: Show!
    private var item: NSStatusItem!
    private var watching: AnyCancellable?, screenWatch: AnyCancellable?
    private var web: WebServer?
    private var relay: Relay?
    private var displayLog: DisplayLog?

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Opened at login: straight to the menu bar, no window.
        let event = NSAppleEventManager.shared().currentAppleEvent
        let atLogin = event?.eventID == kAEOpenApplication &&
            event?.paramDescriptor(forKeyword: keyAEPropData)?.enumCodeValue == keyAELaunchedAsLogInItem
        // Windowless in the menu bar is still running: macOS must not quit it quietly, even asleep.
        ProcessInfo.processInfo.disableAutomaticTermination("Lives in the menu bar")
        ProcessInfo.processInfo.disableSuddenTermination()
        Sentia.registerFonts()
        show = Show()
        web = WebServer(show: show)
        relay = Relay(show: show)
        displayLog = DisplayLog()
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

        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        item.button?.image = Self.barIcon
        let barMenu = NSMenu()
        barMenu.delegate = self
        item.menu = barMenu
        watching = show.$asleep.sink { [weak self] asleep in self?.item.button?.appearsDisabled = asleep }
        // The screen visualizer is a window too: the Dock icon while it's up.
        screenWatch = show.$screenOpen.dropFirst().sink { [weak self] open in
            guard let self else { return }
            if open { NSApp.setActivationPolicy(.regular) } else if !self.window.isVisible { NSApp.setActivationPolicy(.accessory) }
        }

        if atLogin {
            show.windowVisible = false
            NSApp.setActivationPolicy(.accessory)
        } else {
            open()
        }
        Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.show.windowVisible = self.window.isVisible && self.window.occlusionState.contains(.visible)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }
    func applicationWillTerminate(_ notification: Notification) { show.shutDown() }  // the lights as they were
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        open()
        return true
    }

    @objc private func open() {
        NSApp.setActivationPolicy(.regular)
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }
    func windowWillClose(_ notification: Notification) { if !show.screenOpen { NSApp.setActivationPolicy(.accessory) } }

    // The app icon's light and beam (make_icon.py) without the tile, as a template for the menu bar.
    private static let barIcon: NSImage = {
        let image = NSImage(size: NSSize(width: 18, height: 18), flipped: true) { _ in
            let beam = NSBezierPath()
            beam.move(to: NSPoint(x: 9 - 1.5, y: 6.5))
            beam.line(to: NSPoint(x: 9 + 1.5, y: 6.5))
            beam.line(to: NSPoint(x: 9 + 7.2, y: 18))
            beam.line(to: NSPoint(x: 9 - 7.2, y: 18))
            beam.close()
            NSGradient(starting: NSColor.black.withAlphaComponent(0.75), ending: NSColor.black.withAlphaComponent(0.08))?
                .draw(in: beam, angle: 90)
            NSColor.black.setFill()
            NSBezierPath(ovalIn: NSRect(x: 9 - 2.9, y: 5.2 - 2.9, width: 5.8, height: 5.8)).fill()
            return true
        }
        image.isTemplate = true
        image.accessibilityDescription = "Show Control"
        return image
    }()

    // The menu bar menu, built fresh each time it opens.
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        menu.addItem(withTitle: show.status, action: nil, keyEquivalent: "")
        if show.status == "Listening to Spotify" && !show.title.isEmpty {
            menu.addItem(withTitle: show.artist.isEmpty ? show.title : "\(show.title) · \(show.artist)", action: nil, keyEquivalent: "")
        }
        menu.addItem(.separator())
        add(menu, "Open Show Control", #selector(open))
        add(menu, "Lights", #selector(toggleLights), on: show.lampsOn)
        add(menu, "Blackout", #selector(toggleBlackout), on: show.blackout)
        add(menu, "Visualizer on screen", #selector(toggleScreen), on: show.screenOpen)
        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit Show Control", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    }
    private func add(_ menu: NSMenu, _ title: String, _ action: Selector, on: Bool? = nil) {
        let i = menu.addItem(withTitle: title, action: action, keyEquivalent: "")
        i.target = self
        if let on { i.state = on ? .on : .off }
    }
    @objc private func toggleLights() { show.lampsOn.toggle() }
    @objc private func toggleBlackout() { show.blackout.toggle() }
    @objc private func toggleScreen() { show.toggleScreen() }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
