/*
 * Copyright (C) 2024 osy
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

import AppKit
import ArgumentParser
import ObjectiveC

let DEFAULT_VDAGENTD_SOCKET = "/run/spice-vdagentd/spice-vdagent-sock"

typealias VdagentConnection = OpaquePointer

@main
struct Vdagent: ParsableCommand {
    static var configuration = CommandConfiguration(
        commandName: "spice-vdagent")

    @Flag(name: .shortAndLong, help: "Enable debug")
    var debug = false

    @Option(name: [.customLong("vdagentd-socket"), .customShort("S")], help: ArgumentHelp("Set spice-vdagentd socket", valueName: "path"))
    var socketPath = DEFAULT_VDAGENTD_SOCKET

    @Flag(name: [.customLong("foreground"), .customShort("x")], help: "Do not daemonize the agent")
    var foreground = false

    func startVdagent() throws {
        let cb = vdagent_cb_t(clipboard_request: host_clipboard_request,
                              clipboard_grab: host_clipboard_grab,
                              clipboard_data: host_clipboard_data,
                              clipboard_release: host_clipboard_release,
                              client_disconnected: host_client_disconnected,
                              agent_connected: host_agent_connected,
                              agent_disconnected: host_agent_disconnected)
        let host = VdagentHost()

        if debug {
            vdagent_set_debug(1)
        }

        if !foreground {
            throw VdagentError.unimplementedDaemon
        }

        let ctx = Unmanaged.passRetained(host)
        defer {
            ctx.release()
        }
        withUnsafePointer(to: cb) { cb in
            _ = vdagent_start(socketPath, cb, ctx.toOpaque())
        }
    }

    func run() {
        let vdagentQueue = DispatchQueue(label: "SPICE vdagent main loop", qos: .userInteractive)
        vdagentQueue.async {
            do {
                try self.startVdagent()
                Self.exit()
            } catch {
                Self.exit(withError: error)
            }
        }
        RunLoop.current.run()
    }
}

private class VdagentHost: NSObject {
    typealias Item = (lock: DispatchSemaphore, item: NSPasteboardItem)
    private var lastItem: Item?
    private var lastUpdateCount: Int = -1
    private var updateTimer: Timer?
    private var lastAgent: VdagentConnection? {
        willSet {
            if let agent = lastAgent {
                vdagent_unref(agent)
            }
        }
    }

    func clipboardRequest(_ agent: VdagentConnection, sel: vdagent_clipboard_select_t, type: vdagent_clipboard_type_t) throws {
        let pasteboard = try pasteboard(for: sel)
        let pasteboardType = try pasteboardType(for: type)
        guard let data = pasteboard.data(forType: pasteboardType) else {
            throw VdagentError.dataNotAvailable
        }
        try data.withUnsafeBytes<Void> { buffer in
            vdagent_clipboard_data(agent, sel, type, buffer.baseAddress, UInt32(data.count))
        }
    }

    func clipboardGrab(_ agent: VdagentConnection, sel: vdagent_clipboard_select_t, types: [vdagent_clipboard_type_t]) throws {
        let pasteboard = try pasteboard(for: sel)
        let pasteboardTypes = types.compactMap { try? pasteboardType(for: $0) }
        let item = NSPasteboardItem()
        item.setDataProvider(self, forTypes: pasteboardTypes)
        item.connectionAgent = agent
        lastUpdateCount = pasteboard.changeCount + 1
        pasteboard.prepareForNewContents(with: .currentHostOnly)
        pasteboard.writeObjects([item])
    }

    func clipboardData(_ agent: VdagentConnection, sel: vdagent_clipboard_select_t, type: vdagent_clipboard_type_t, data: Data) throws {
        let pasteboard = try pasteboard(for: sel)
        let pasteboardType = try pasteboardType(for: type)
        let success: Bool
        if let (lock, item) = lastItem {
            lastItem = nil
            if item.types.contains(pasteboardType) {
                success = item.setData(data, forType: pasteboardType)
            } else {
                success = false
            }
            lock.signal()
        } else {
            // unsolicited clipboard data
            lastUpdateCount = pasteboard.changeCount + 1
            success = pasteboard.setData(data, forType: pasteboardType)
        }
        if !success {
            throw VdagentError.failedToWritePasteboard
        }
    }

    func clipboardRelease(_ agent: VdagentConnection, sel: vdagent_clipboard_select_t) throws {
    }

    func guestUpdate(_ agent: VdagentConnection) {
        let pasteboard = NSPasteboard.general
        let newCount = pasteboard.changeCount
        if newCount != lastUpdateCount, let pasteboardTypes = pasteboard.types {
            lastUpdateCount = newCount
            let types = pasteboardTypes.compactMap { clipboardType(for: $0) }
            if !types.isEmpty {
                vdagent_clipboard_grab(agent, VDAgentClipboardSelectClipboard, types, Int32(types.count))
            }
        }
    }

    func clientDisconnected(_ agent: VdagentConnection) {
    }

    func agentConnected(_ agent: VdagentConnection) {
        let agent = vdagent_ref(agent)!
        // need to synchronize with RunLoop
        RunLoop.main.perform {
            self.lastAgent = agent
            let timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
                self?.guestUpdate(agent)
            }
            self.updateTimer = timer
            RunLoop.main.add(timer, forMode: .default)
        }
    }

    func agentDisconnected(_ agent: VdagentConnection) {
        updateTimer?.invalidate()
        // need to synchronize with RunLoop
        RunLoop.main.perform {
            self.updateTimer = nil
            self.lastAgent = nil
        }
    }
}

private extension VdagentHost {
    private func pasteboard(for sel: vdagent_clipboard_select_t) throws -> NSPasteboard {
        switch sel {
        case VDAgentClipboardSelectClipboard: return .general
        default: throw VdagentError.unsupportedClipboardSelection(sel)
        }
    }

    private func pasteboardType(for type: vdagent_clipboard_type_t) throws -> NSPasteboard.PasteboardType {
        switch type {
        case VDAgentClipboardTypeImageTIFF: return .tiff
        case VDAgentClipboardTypeImagePNG: return .png
        case VDAgentClipboardTypeUTF8Text: return .string
        default: throw VdagentError.unsupportedDataType(type)
        }
    }

    private func clipboardType(for type: NSPasteboard.PasteboardType) -> vdagent_clipboard_type_t? {
        switch type {
        case .tiff: return VDAgentClipboardTypeImageTIFF
        case .png: return VDAgentClipboardTypeImagePNG
        case .string: return VDAgentClipboardTypeUTF8Text
        default: return nil
        }
    }
}

extension VdagentHost: NSPasteboardItemDataProvider {
    func pasteboard(_ pasteboard: NSPasteboard?, item: NSPasteboardItem, provideDataForType type: NSPasteboard.PasteboardType) {
        guard let type = clipboardType(for: type) else {
            print("Error: unsupported pasteboard type \(type)")
            return
        }
        lastItem = (DispatchSemaphore(value: 0), item)
        vdagent_clipboard_request(item.connectionAgent, VDAgentClipboardSelectClipboard, type)
        guard lastItem!.lock.wait(timeout: .now() + .seconds(5)) == .success else {
            lastItem = nil
            print("Error: timed out waiting for clipboard data")
            return
        }
    }
}

private enum VdagentError: Error {
    case unimplementedDaemon
    case unsupportedClipboardSelection(vdagent_clipboard_select_t)
    case unsupportedDataType(vdagent_clipboard_type_t)
    case dataNotAvailable
    case failedToWritePasteboard
}

extension VdagentError: LocalizedError {
    var errorDescription: String? {
        switch self {
        case .unimplementedDaemon:
            return "Daemon mode is not implemented, please run with '-x'."
        case .unsupportedClipboardSelection(let sel):
            return "Unsupported clipboard selection: \(sel)"
        case .unsupportedDataType(let type):
            return "Unsupported clipboard data type: \(type)"
        case .dataNotAvailable:
            return "Data not available"
        case .failedToWritePasteboard:
            return "Failed to write to pasteboard"
        }
    }
}

private var AssociatedAgentConnection: UInt8 = 0

extension NSPasteboardItem {
    var connectionAgent: VdagentConnection? {
        get {
            return objc_getAssociatedObject(self, &AssociatedAgentConnection) as? VdagentConnection
        }

        set {
            objc_setAssociatedObject(self, &AssociatedAgentConnection, newValue,
                                     objc_AssociationPolicy.OBJC_ASSOCIATION_RETAIN_NONATOMIC)
        }
    }
}

private func withError(_ callback: () throws -> Void) -> Bool {
    do {
        try callback()
        return true
    } catch {
        print("Error: \(error.localizedDescription)")
        return false
    }
}

private func host_clipboard_request(_ agent: VdagentConnection!, ctx: UnsafeMutableRawPointer!, sel: vdagent_clipboard_select_t, type: vdagent_clipboard_type_t) -> Bool {
    withError {
        let host = Unmanaged<VdagentHost>.fromOpaque(ctx).takeUnretainedValue()
        try host.clipboardRequest(agent, sel: sel, type: type)
    }
}

private func host_clipboard_grab(_ agent: VdagentConnection!, ctx: UnsafeMutableRawPointer!, sel: vdagent_clipboard_select_t, types: UnsafePointer<vdagent_clipboard_type_t>?, n_types: UInt32) -> Bool {
    withError {
        let host = Unmanaged<VdagentHost>.fromOpaque(ctx).takeUnretainedValue()
        let _types = Array(UnsafeBufferPointer(start: types!, count: Int(n_types)))
        try host.clipboardGrab(agent, sel: sel, types: _types)
    }
}

private func host_clipboard_data(_ agent: VdagentConnection!, ctx: UnsafeMutableRawPointer!, sel: vdagent_clipboard_select_t, type: vdagent_clipboard_type_t, data: UnsafePointer<UInt8>?, size: UInt32) -> Bool {
    withError {
        let host = Unmanaged<VdagentHost>.fromOpaque(ctx).takeUnretainedValue()
        let _data: Data
        if let data = data {
            _data = Data(bytes: data, count: Int(size))
        } else {
            _data = Data()
        }
        try host.clipboardData(agent, sel: sel, type: type, data: _data)
    }
}

private func host_clipboard_release(_ agent: VdagentConnection!, ctx: UnsafeMutableRawPointer!, sel: vdagent_clipboard_select_t) -> Bool {
    withError {
        let host = Unmanaged<VdagentHost>.fromOpaque(ctx).takeUnretainedValue()
        try host.clipboardRelease(agent, sel: sel)
    }
}

private func host_client_disconnected(_ agent: VdagentConnection!, ctx: UnsafeMutableRawPointer!) {
    let host = Unmanaged<VdagentHost>.fromOpaque(ctx).takeUnretainedValue()
    host.clientDisconnected(agent)
}

private func host_agent_connected(_ agent: VdagentConnection!, ctx: UnsafeMutableRawPointer!) {
    let host = Unmanaged<VdagentHost>.fromOpaque(ctx).takeUnretainedValue()
    host.agentConnected(agent)
}

private func host_agent_disconnected(_ agent: VdagentConnection!, ctx: UnsafeMutableRawPointer!) {
    let host = Unmanaged<VdagentHost>.fromOpaque(ctx).takeUnretainedValue()
    host.agentDisconnected(agent)
}
