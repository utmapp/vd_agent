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

    mutating func run() throws {
        let cb = vdagent_cb_t(clipboard_request: host_clipboard_request,
                              clipboard_grab: host_clipboard_grab,
                              clipboard_data: host_clipboard_data,
                              clipboard_release: host_clipboard_release,
                              clipboard_guest_update: host_guest_update,
                              client_disconnected: host_client_disconnected)
        var host = VdagentHost()

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
}

private class VdagentHost: NSObject {
    func clipboardRequest(_ agent: VdagentConnection, sel: vdagent_clipboard_select_t, type: vdagent_clipboard_type_t) throws {
    }

    func clipboardGrab(_ agent: VdagentConnection, sel: vdagent_clipboard_select_t, types: [vdagent_clipboard_type_t]) throws {
    }

    func clipboardData(_ agent: VdagentConnection, sel: vdagent_clipboard_select_t, type: vdagent_clipboard_type_t, data: Data) throws {
    }

    func clipboardRelease(_ agent: VdagentConnection, sel: vdagent_clipboard_select_t) throws {
    }

    func guestUpdate(_ agent: VdagentConnection) {

    }

    func clientDisconnected(_ agent: VdagentConnection) {
    }
}

private extension VdagentHost {
    private func pasteboard(for sel: vdagent_clipboard_select_t) throws -> NSPasteboard {
        switch sel {
        case VDAgentClipboardSelectPrimary: return .general
        default: throw VdagentError.unsupportedClipboardSelection(sel)
        }
    }

    private func pasteboardType(for type: vdagent_clipboard_type_t) -> NSPasteboard.PasteboardType? {
        switch type {
        case VDAgentClipboardTypeImageTIFF: return .tiff
        case VDAgentClipboardTypeImagePNG: return .png
        case VDAgentClipboardTypeUTF8Text: return .string
        default: return nil
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

    }
}

private enum VdagentError: Error {
    case unimplementedDaemon
    case unsupportedClipboardSelection(vdagent_clipboard_select_t)
}

extension VdagentError: LocalizedError {
    var errorDescription: String? {
        switch self {
        case .unimplementedDaemon:
            return "Daemon mode is not implemented, please run with '-x'."
        case .unsupportedClipboardSelection(let sel):
            return "Unsupported clipboard selection: \(sel)"
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
        let _data = Data(bytes: data!, count: Int(size))
        try host.clipboardData(agent, sel: sel, type: type, data: _data)
    }
}

private func host_clipboard_release(_ agent: VdagentConnection!, ctx: UnsafeMutableRawPointer!, sel: vdagent_clipboard_select_t) -> Bool {
    withError {
        let host = Unmanaged<VdagentHost>.fromOpaque(ctx).takeUnretainedValue()
        try host.clipboardRelease(agent, sel: sel)
    }
}

private func host_guest_update(_ agent: VdagentConnection!, ctx: UnsafeMutableRawPointer!) {
    let host = Unmanaged<VdagentHost>.fromOpaque(ctx).takeUnretainedValue()
    host.guestUpdate(agent)
}

private func host_client_disconnected(_ agent: VdagentConnection!, ctx: UnsafeMutableRawPointer!) {
    let host = Unmanaged<VdagentHost>.fromOpaque(ctx).takeUnretainedValue()
    host.clientDisconnected(agent)
}
