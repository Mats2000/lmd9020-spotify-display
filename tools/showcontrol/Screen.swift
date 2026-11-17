// The visualizer on this Mac's own screen: a full-screen window running screen/index.html
// (three.js scenes drawn low-res and dithered), fed the display's own music data, kicks and the
// cover's colours. Keys and the pointer play it; Esc or leaving full screen closes it.

import AppKit
import WebKit

final class ScreenShow: NSObject, WKScriptMessageHandler, NSWindowDelegate {
    private var window: NSWindow?
    private var web: WKWebView?
    private var ready = false
    private var song: [Any] = ["", "", [[Int]]()]
    private var settings: [String: Any] = [:]
    // Read off the main queue before hopping on, so a closed screen costs nothing per packet.
    private(set) var wanted = false
    var onOpen: ((Bool) -> Void)?
    var onSettings: (([String: Any]) -> Void)?  // changed from the keys (scene, res, caption)

    func open(on screen: NSScreen) {
        if let window { window.makeKeyAndOrderFront(nil); return }
        let config = WKWebViewConfiguration()
        config.userContentController.add(self, name: "screen")  // removed on close, or it holds us
        let web = WKWebView(frame: NSRect(origin: .zero, size: screen.frame.size), configuration: config)
        web.setValue(false, forKey: "drawsBackground")
        let w = NSWindow(contentRect: screen.frame, styleMask: [.titled, .closable, .resizable, .fullSizeContentView],
                         backing: .buffered, defer: false, screen: screen)
        w.title = "Show Control Screen"
        w.titleVisibility = .hidden
        w.titlebarAppearsTransparent = true
        w.backgroundColor = .black
        w.isReleasedWhenClosed = false
        w.collectionBehavior = [.fullScreenPrimary]
        w.contentView = web
        w.delegate = self
        w.setFrame(screen.frame, display: true)
        let res = Bundle.main.resourceURL!
        web.loadFileURL(res.appendingPathComponent("screen/index.html"), allowingReadAccessTo: res)
        window = w
        self.web = web
        wanted = true
        w.makeKeyAndOrderFront(nil)
        w.makeFirstResponder(web)
        NSApp.activate(ignoringOtherApps: true)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { w.toggleFullScreen(nil) }
        onOpen?(true)
    }

    func close() { window?.close() }

    func windowWillClose(_ notification: Notification) {
        web?.configuration.userContentController.removeScriptMessageHandler(forName: "screen")
        window?.contentView = nil
        web = nil
        window = nil
        ready = false
        wanted = false
        onOpen?(false)
    }
    func windowDidExitFullScreen(_ notification: Notification) { DispatchQueue.main.async { self.window?.close() } }

    func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
        guard let m = message.body as? [String: Any] else { return }
        if m["ready"] != nil {
            ready = true
            apply(settings)
            call("song", song)
        }
        if m["close"] != nil { close() }
        var changed = m
        changed["ready"] = nil
        changed["close"] = nil
        if !changed.isEmpty {
            settings.merge(changed) { $1 }
            onSettings?(changed)
        }
    }

    // MARK: To the page

    func apply(_ s: [String: Any]) {
        settings = s
        call("configure", [s])
    }
    func setSong(title: String, artist: String, colours: [RGB]) {
        song = [title, artist, colours.map { [Int($0.r), Int($0.g), Int($0.b)] }]
        call("song", song)
    }
    // A display packet ("LMDV": level at 6, beat 7, 16 bands from 8, 64 signed wave samples from 26).
    func feed(_ p: [UInt8]) {
        guard ready, p.count >= 90 else { return }
        let bands = p[8..<24].map(String.init).joined(separator: ","),
            wave = p[26..<90].map { String(Int8(bitPattern: $0)) }.joined(separator: ",")
        web?.evaluateJavaScript("feed(\(p[6]),\(p[7]),[\(bands)],[\(wave)])", completionHandler: nil)
    }
    func kick(_ s: Float) { if ready { web?.evaluateJavaScript("kick(\(s))", completionHandler: nil) } }
    func accent(_ s: Float) { if ready { web?.evaluateJavaScript("accent(\(s))", completionHandler: nil) } }
    func impact() { if ready { web?.evaluateJavaScript("impact()", completionHandler: nil) } }
    func flash(_ on: Bool) { if ready { web?.evaluateJavaScript("flash(\(on))", completionHandler: nil) } }

    private func call(_ fn: String, _ args: [Any]) {
        guard ready, let data = try? JSONSerialization.data(withJSONObject: args), let json = String(data: data, encoding: .utf8) else { return }
        web?.evaluateJavaScript("\(fn)(...\(json))", completionHandler: nil)
    }
}
