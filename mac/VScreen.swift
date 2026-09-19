// VScreen: full-screen receiver for vscreen-send.
//
// Listens on TCP (default port 7310), accepts one sender at a time, and shows
// its H.264 stream in an AVSampleBufferDisplayLayer, which decodes on the GPU
// and presents each frame as soon as it is decoded. See linux/protocol.h for
// the wire format.

import AVFoundation
import Cocoa
import CoreMedia
import Network

let vsMagic: UInt32 = 0x5653_4352
let vsHeaderLen = 20
let vsMsgHello: UInt8 = 1
let vsMsgVideo: UInt8 = 2
let vsMsgKeyframeRequest: UInt8 = 3
let vsFlagKeyframe: UInt8 = 1

// MARK: - H.264 -> AVSampleBufferDisplayLayer

final class VideoSink {
    let layer: AVSampleBufferDisplayLayer
    var requestKeyframe: () -> Void = {}

    private var sps: [UInt8] = []
    private var pps: [UInt8] = []
    private var format: CMVideoFormatDescription?
    private var needKeyframe = true
    private var lastKeyframeRequest = Date.distantPast

    init(layer: AVSampleBufferDisplayLayer) {
        self.layer = layer
    }

    func reset() {
        sps = []
        pps = []
        format = nil
        needKeyframe = true
    }

    /// `accessUnit` is Annex-B: NAL units separated by 00 00 01 / 00 00 00 01.
    func decode(_ accessUnit: UnsafeBufferPointer<UInt8>) {
        var avcc = [UInt8]()
        avcc.reserveCapacity(accessUnit.count + 32)
        var newSPS: [UInt8]?
        var newPPS: [UInt8]?
        var hasIDR = false

        forEachNAL(accessUnit) { nal in
            switch nal[0] & 0x1F {
            case 7: newSPS = Array(nal)
            case 8: newPPS = Array(nal)
            case 9: break  // access unit delimiter
            case let type:
                if type == 5 { hasIDR = true }
                let n = UInt32(nal.count)
                avcc += [UInt8(n >> 24), UInt8(n >> 16 & 0xFF), UInt8(n >> 8 & 0xFF), UInt8(n & 0xFF)]
                avcc += nal
            }
        }

        if let s = newSPS, let p = newPPS, s != sps || p != pps {
            sps = s
            pps = p
            format = makeFormat()
        }
        guard let format, !avcc.isEmpty else {
            askForKeyframe()
            return
        }
        if layer.status == .failed {
            NSLog("VScreen: decoder failed: \(String(describing: layer.error))")
            layer.flush()
            needKeyframe = true
        }
        if needKeyframe {
            guard hasIDR else {
                askForKeyframe()
                return
            }
            needKeyframe = false
        }
        if let sample = makeSample(avcc, format: format) {
            layer.enqueue(sample)
        }
    }

    private func askForKeyframe() {
        let now = Date()
        if now.timeIntervalSince(lastKeyframeRequest) > 0.5 {
            lastKeyframeRequest = now
            requestKeyframe()
        }
    }

    private func makeFormat() -> CMVideoFormatDescription? {
        var fmt: CMFormatDescription?
        let status = sps.withUnsafeBufferPointer { s in
            pps.withUnsafeBufferPointer { p in
                CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    allocator: kCFAllocatorDefault,
                    parameterSetCount: 2,
                    parameterSetPointers: [s.baseAddress!, p.baseAddress!],
                    parameterSetSizes: [s.count, p.count],
                    nalUnitHeaderLength: 4,
                    formatDescriptionOut: &fmt)
            }
        }
        if status != noErr {
            NSLog("VScreen: bad SPS/PPS (\(status))")
            return nil
        }
        return fmt
    }

    private func makeSample(_ avcc: [UInt8], format: CMVideoFormatDescription) -> CMSampleBuffer? {
        var block: CMBlockBuffer?
        guard CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault, memoryBlock: nil, blockLength: avcc.count,
            blockAllocator: kCFAllocatorDefault, customBlockSource: nil, offsetToData: 0,
            dataLength: avcc.count, flags: kCMBlockBufferAssureMemoryNowFlag,
            blockBufferOut: &block) == noErr, let block
        else { return nil }
        avcc.withUnsafeBytes { bytes in
            _ = CMBlockBufferReplaceDataBytes(
                with: bytes.baseAddress!, blockBuffer: block, offsetIntoDestination: 0,
                dataLength: avcc.count)
        }

        var sample: CMSampleBuffer?
        var size = avcc.count
        guard CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault, dataBuffer: block, formatDescription: format,
            sampleCount: 1, sampleTimingEntryCount: 0, sampleTimingArray: nil,
            sampleSizeEntryCount: 1, sampleSizeArray: &size, sampleBufferOut: &sample) == noErr,
            let sample
        else { return nil }

        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true),
            CFArrayGetCount(attachments) > 0
        {
            let dict = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFMutableDictionary.self)
            CFDictionarySetValue(
                dict,
                Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(),
                Unmanaged.passUnretained(kCFBooleanTrue).toOpaque())
        }
        return sample
    }
}

/// Calls `body` for each NAL unit (without start code or trailing zeros).
func forEachNAL(_ data: UnsafeBufferPointer<UInt8>, _ body: (UnsafeBufferPointer<UInt8>) -> Void) {
    let count = data.count
    var nalStart = -1
    var i = 0

    func emit(_ start: Int, _ end: Int) {
        var end = end
        while end > start && data[end - 1] == 0 { end -= 1 }
        if end > start {
            body(UnsafeBufferPointer(rebasing: data[start..<end]))
        }
    }

    while i + 2 < count {
        if data[i + 2] > 1 {
            i += 3
        } else if data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 {
            if nalStart >= 0 { emit(nalStart, i) }
            i += 3
            nalStart = i
        } else {
            i += 1
        }
    }
    if nalStart >= 0 { emit(nalStart, count) }
}

// MARK: - Network

struct Stats {
    var frames = 0
    var bytes = 0
    var width = 0
    var height = 0
}

final class Receiver {
    enum State {
        case waiting
        case connected(String)
    }

    let queue = DispatchQueue(label: "vscreen.receiver", qos: .userInteractive)
    let sink: VideoSink
    let port: UInt16
    var onState: (State) -> Void = { _ in }

    private var listener: NWListener?
    private var connection: NWConnection?
    private var buffer = [UInt8]()
    private var readPos = 0
    private var stats = Stats()
    private let statsLock = NSLock()

    init(port: UInt16, sink: VideoSink) {
        self.port = port
        self.sink = sink
        sink.requestKeyframe = { [weak self] in self?.sendKeyframeRequest() }
    }

    func start() throws {
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        let params = NWParameters(tls: nil, tcp: tcp)
        params.allowLocalEndpointReuse = true
        let listener = try NWListener(using: params, on: NWEndpoint.Port(rawValue: port)!)
        listener.newConnectionHandler = { [weak self] c in self?.accept(c) }
        listener.stateUpdateHandler = { state in
            if case .failed(let error) = state {
                NSLog("VScreen: listener failed: \(error)")
            }
        }
        listener.start(queue: queue)
        self.listener = listener
    }

    /// Frames and bytes since the last call.
    func takeStats() -> Stats {
        statsLock.lock()
        defer { statsLock.unlock() }
        let s = stats
        stats.frames = 0
        stats.bytes = 0
        return s
    }

    private func accept(_ c: NWConnection) {
        if let old = connection {
            NSLog("VScreen: replacing existing sender")
            old.cancel()
        }
        connection = c
        buffer.removeAll(keepingCapacity: true)
        readPos = 0
        sink.reset()

        let peer = "\(c.endpoint)"
        c.stateUpdateHandler = { [weak self, weak c] state in
            guard let self, let c, self.connection === c else { return }
            switch state {
            case .ready:
                self.onState(.connected(peer))
            case .failed, .cancelled:
                self.connection = nil
                self.onState(.waiting)
            default:
                break
            }
        }
        c.start(queue: queue)
        receive(on: c)
    }

    private func receive(on c: NWConnection) {
        c.receive(minimumIncompleteLength: 1, maximumLength: 4 << 20) { [weak self] data, _, done, error in
            guard let self, self.connection === c else { return }
            if let data {
                self.buffer.append(contentsOf: data)
                if !self.parse() {
                    c.cancel()
                    return
                }
            }
            if done || error != nil {
                c.cancel()
                return
            }
            self.receive(on: c)
        }
    }

    /// Consume complete messages. Returns false on a protocol error.
    private func parse() -> Bool {
        var ok = true
        buffer.withUnsafeBufferPointer { buf in
            while buf.count - readPos >= vsHeaderLen {
                let h = readPos
                guard be32(buf, h) == vsMagic else {
                    NSLog("VScreen: bad magic, dropping connection")
                    ok = false
                    return
                }
                let type = buf[h + 4]
                let flags = buf[h + 5]
                let length = Int(be32(buf, h + 8))
                guard length <= 64 << 20 else {
                    ok = false
                    return
                }
                guard buf.count - readPos >= vsHeaderLen + length else { break }
                let payload = UnsafeBufferPointer(rebasing: buf[(h + vsHeaderLen)..<(h + vsHeaderLen + length)])
                handle(type: type, flags: flags, payload: payload)
                readPos += vsHeaderLen + length
            }
        }
        if readPos == buffer.count {
            buffer.removeAll(keepingCapacity: true)
            readPos = 0
        } else if readPos > 1 << 20 {
            buffer.removeFirst(readPos)
            readPos = 0
        }
        return ok
    }

    private func handle(type: UInt8, flags: UInt8, payload: UnsafeBufferPointer<UInt8>) {
        switch type {
        case vsMsgHello where payload.count >= 12:
            let w = Int(be32(payload, 4))
            let h = Int(be32(payload, 8))
            NSLog("VScreen: sender stream \(w)x\(h)")
            statsLock.lock()
            stats.width = w
            stats.height = h
            statsLock.unlock()
        case vsMsgVideo:
            sink.decode(payload)
            statsLock.lock()
            stats.frames += 1
            stats.bytes += payload.count
            statsLock.unlock()
        default:
            break
        }
    }

    private func sendKeyframeRequest() {
        guard let c = connection else { return }
        var msg = [UInt8](repeating: 0, count: vsHeaderLen)
        msg[0] = 0x56; msg[1] = 0x53; msg[2] = 0x43; msg[3] = 0x52
        msg[4] = vsMsgKeyframeRequest
        c.send(content: Data(msg), completion: .contentProcessed { _ in })
    }
}

func be32(_ b: UnsafeBufferPointer<UInt8>, _ o: Int) -> UInt32 {
    UInt32(b[o]) << 24 | UInt32(b[o + 1]) << 16 | UInt32(b[o + 2]) << 8 | UInt32(b[o + 3])
}

func localIPv4Addresses() -> [String] {
    var result = [String]()
    var ifaddr: UnsafeMutablePointer<ifaddrs>?
    guard getifaddrs(&ifaddr) == 0, let first = ifaddr else { return result }
    defer { freeifaddrs(ifaddr) }
    for ptr in sequence(first: first, next: { $0.pointee.ifa_next }) {
        let ifa = ptr.pointee
        guard let addr = ifa.ifa_addr, addr.pointee.sa_family == UInt8(AF_INET) else { continue }
        let name = String(cString: ifa.ifa_name)
        guard name != "lo0" else { continue }
        var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
        if getnameinfo(addr, socklen_t(addr.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 {
            result.append("\(String(cString: host))  (\(name))")
        }
    }
    return result
}

// MARK: - UI

final class ScreenView: NSView {
    let videoLayer = AVSampleBufferDisplayLayer()
    let label = NSTextField(wrappingLabelWithString: "")
    private let blankCursor = NSCursor(image: NSImage(size: NSSize(width: 1, height: 1)), hotSpot: .zero)

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        videoLayer.videoGravity = .resizeAspect
        videoLayer.backgroundColor = NSColor.black.cgColor
        layer?.addSublayer(videoLayer)

        label.textColor = NSColor(white: 0.85, alpha: 1)
        label.font = .monospacedSystemFont(ofSize: 22, weight: .regular)
        label.alignment = .center
        label.translatesAutoresizingMaskIntoConstraints = false
        addSubview(label)
        NSLayoutConstraint.activate([
            label.centerXAnchor.constraint(equalTo: centerXAnchor),
            label.centerYAnchor.constraint(equalTo: centerYAnchor),
            label.widthAnchor.constraint(lessThanOrEqualTo: widthAnchor, constant: -80),
        ])
    }

    required init?(coder: NSCoder) { fatalError() }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        videoLayer.frame = bounds
        CATransaction.commit()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        videoLayer.contentsScale = window?.backingScaleFactor ?? 2
    }

    override func resetCursorRects() {
        addCursorRect(bounds, cursor: blankCursor)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    var window: NSWindow!
    var view: ScreenView!
    var receiver: Receiver!
    var sink: VideoSink!
    var connectedPeer: String?
    var showStats = false
    var sharpScaling = true
    var displayActivity: NSObjectProtocol?
    var lastStats = Stats()

    var port: UInt16 {
        let p = UserDefaults.standard.integer(forKey: "port")
        return p > 0 && p < 65536 ? UInt16(p) : 7310
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildMenu()

        let screen = NSScreen.main ?? NSScreen.screens[0]
        view = ScreenView(frame: screen.frame)
        window = NSWindow(contentRect: screen.frame, styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false, screen: screen)
        window.title = "VScreen"
        window.backgroundColor = .black
        window.collectionBehavior = [.fullScreenPrimary]
        window.contentView = view
        window.makeKeyAndOrderFront(nil)
        applyScaling()

        sink = VideoSink(layer: view.videoLayer)
        receiver = Receiver(port: port, sink: sink)
        receiver.onState = { [weak self] state in
            DispatchQueue.main.async { self?.update(state) }
        }
        do {
            try receiver.start()
        } catch {
            view.label.stringValue = "Cannot listen on port \(port): \(error)"
            return
        }
        update(.waiting)

        Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.tick() }

        if !UserDefaults.standard.bool(forKey: "windowed") {
            window.toggleFullScreen(nil)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    private func update(_ state: Receiver.State) {
        switch state {
        case .waiting:
            connectedPeer = nil
            view.videoLayer.flushAndRemoveImage()
            if let a = displayActivity {
                ProcessInfo.processInfo.endActivity(a)
                displayActivity = nil
            }
        case .connected(let peer):
            connectedPeer = peer
            if displayActivity == nil {
                displayActivity = ProcessInfo.processInfo.beginActivity(
                    options: [.idleDisplaySleepDisabled, .userInitiated, .latencyCritical],
                    reason: "Showing remote screen")
            }
        }
        refreshLabel()
    }

    private func tick() {
        lastStats = receiver.takeStats()
        if showStats || connectedPeer == nil { refreshLabel() }
    }

    private func refreshLabel() {
        guard connectedPeer != nil else {
            let ips = localIPv4Addresses()
            view.label.stringValue = """
                VScreen — waiting for the laptop on port \(port)

                On the laptop run:  vscreen-send <address>

                This Mac:
                \(ips.isEmpty ? "(no network)" : ips.joined(separator: "\n"))
                """
            view.label.isHidden = false
            return
        }
        if showStats {
            let s = lastStats
            view.label.stringValue = String(
                format: "%@\n%dx%d  %d fps  %.1f Mbit/s", connectedPeer!, s.width, s.height, s.frames,
                Double(s.bytes) * 8 / 1_000_000)
        }
        view.label.isHidden = !showStats
    }

    private func applyScaling() {
        let filter: CALayerContentsFilter = sharpScaling ? .nearest : .linear
        view.videoLayer.magnificationFilter = filter
        view.videoLayer.minificationFilter = .linear
    }

    @objc func toggleStats(_ sender: NSMenuItem) {
        showStats.toggle()
        sender.state = showStats ? .on : .off
        refreshLabel()
    }

    @objc func toggleSharp(_ sender: NSMenuItem) {
        sharpScaling.toggle()
        sender.state = sharpScaling ? .on : .off
        applyScaling()
    }

    private func buildMenu() {
        let main = NSMenu()

        let appItem = NSMenuItem()
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "Hide VScreen", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Quit VScreen", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        main.addItem(appItem)

        let viewItem = NSMenuItem()
        let viewMenu = NSMenu(title: "View")
        let fs = viewMenu.addItem(withTitle: "Toggle Full Screen", action: #selector(NSWindow.toggleFullScreen(_:)), keyEquivalent: "f")
        fs.keyEquivalentModifierMask = [.command, .control]
        viewMenu.addItem(withTitle: "Show Stats", action: #selector(toggleStats(_:)), keyEquivalent: "i").target = self
        let sharp = viewMenu.addItem(withTitle: "Sharp Scaling", action: #selector(toggleSharp(_:)), keyEquivalent: "s")
        sharp.target = self
        sharp.state = .on
        viewItem.submenu = viewMenu
        main.addItem(viewItem)

        NSApp.mainMenu = main
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.activate(ignoringOtherApps: true)
app.run()
