/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

import Cocoa

private class EmbeddedView: View {
    private var screenObserver: NSObjectProtocol?

    deinit {
        if let screenObserver {
            NotificationCenter.default.removeObserver(screenObserver)
        }
    }

    override func setFrameSize(_ newSize: NSSize) {
        let changed = frame.size != newSize
        super.setFrameSize(newSize)
        if changed {
            (common as? MacCommon)?.embeddedViewDidResize()
        }
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if let screenObserver {
            NotificationCenter.default.removeObserver(screenObserver)
            self.screenObserver = nil
        }
        if let window {
            screenObserver = NotificationCenter.default.addObserver(
                forName: NSWindow.didChangeScreenNotification,
                object: window,
                queue: .main
            ) { [weak self] _ in
                (self?.common as? MacCommon)?.embeddedViewDidMoveToWindow()
            }
        }
        (common as? MacCommon)?.embeddedViewDidMoveToWindow()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        (common as? MacCommon)?.embeddedViewDidChangeBackingProperties()
    }
}

class MacCommon: Common {
    @objc var layer: MetalLayer?

    private var embeddedHostView: NSView?

    var presentation: Presentation?
    var timer: PreciseTimer?
    var swapTime: UInt64 = 0
    let swapLock: NSCondition = NSCondition()

    @objc init(_ vo: UnsafeMutablePointer<vo>) {
        let log = LogHelper(mp_log_new(vo, vo.pointee.log, "mac"))
        let option = OptionHelper(vo, vo.pointee.global)
        super.init(option, log)
        eventsLock.withLock { self.vo = vo }
        input = InputHelper(vo.pointee.input_ctx, option)
        presentation = Presentation(common: self)
        timer = PreciseTimer(common: self)

        DispatchQueue.main.sync {
            layer = MetalLayer(common: self)
            if let hostView = resolveEmbeddedHostView(), let layer = layer {
                embeddedHostView = hostView
                let embeddedView = EmbeddedView(frame: hostView.bounds, common: self)
                embeddedView.layer = layer
                embeddedView.wantsLayer = true
                embeddedView.layerContentsPlacement = .scaleProportionallyToFit
                layer.delegate = embeddedView
                layer.contentsScale = hostView.window?.backingScaleFactor ?? 1
                view = embeddedView
                hostView.addSubview(embeddedView)
                log.verbose("Embedding macvk output in external NSView: \(hostView)")
            }
            initMisc(vo)
        }
    }

    private func resolveEmbeddedHostView() -> NSView? {
        let windowID = option.vo.WinID
        guard windowID > 0,
              let pointer = UnsafeRawPointer(bitPattern: Int(windowID)) else {
            return nil
        }

        let object = Unmanaged<AnyObject>.fromOpaque(pointer).takeUnretainedValue()
        guard let hostView = object as? NSView else {
            log.error("macOS --wid must point to an NSView")
            return nil
        }
        return hostView
    }

    @objc func config(_ vo: UnsafeMutablePointer<vo>) -> Bool {
        eventsLock.withLock { self.vo = vo }

        if let hostView = embeddedHostView {
            DispatchQueue.main.sync {
                view?.frame = hostView.bounds
                layer?.contentsScale = hostView.window?.backingScaleFactor ?? 1
                windowDidResize()
                updateICCProfile()
            }
            return true
        }

        DispatchQueue.main.sync {
            let previousActiveApp = getActiveApp()
            initApp()

            let (screen, wr, forcePosition) = getInitProperties(vo)
            guard let layer = self.layer else {
                log.error("Something went wrong, no MetalLayer was initialized")
                exit(1)
            }

            if window == nil {
                initView(vo, layer)
                initWindow(vo, previousActiveApp)
                initWindowState()
            }

            if forcePosition {
                window?.updateFrame(wr, screen)
            } else if option.vo.auto_window_resize {
                window?.updateSize(wr.size)
            }

            if option.vo.focus_on == 2 {
                NSApp.activate(ignoringOtherApps: true)
            }

            windowDidResize()
            updateICCProfile()
        }

        return true
    }

    @objc func uninit(_ vo: UnsafeMutablePointer<vo>) {
        if embeddedHostView == nil {
            window?.waitForAnimation()
        }

        timer?.terminate()

        DispatchQueue.main.sync {
            if self.embeddedHostView == nil {
                window?.delegate = nil
                window?.close()
            }

            uninitCommon()
            view = nil
            embeddedHostView = nil
        }
    }

    func embeddedViewDidResize() {
        guard embeddedHostView != nil else { return }
        windowDidResize()
    }

    func embeddedViewDidMoveToWindow() {
        guard embeddedHostView != nil, view?.window != nil else { return }
        if link != nil {
            updateDisplaylink()
        }
        embeddedViewDidChangeBackingProperties()
        updateICCProfile()
    }

    func embeddedViewDidChangeBackingProperties() {
        guard embeddedHostView != nil else { return }
        layer?.contentsScale = view?.window?.backingScaleFactor ?? 1
        windowDidResize()
    }

    @objc func renderSize() -> CGSize {
        var size = CGSize.zero
        let updateSize = {
            if self.embeddedHostView != nil, let view = self.view {
                size = view.convertToBacking(view.bounds).size
            } else {
                size = self.window?.framePixel.size ?? CGSize.zero
            }
        }

        if Thread.isMainThread {
            updateSize()
        } else {
            DispatchQueue.main.sync(execute: updateSize)
        }
        return size
    }

    @objc func swapBuffer() {
        if option.mac.macos_render_timer > RENDER_TIMER_SYSTEM {
            swapLock.lock()
            while swapTime < 1 {
                swapLock.wait()
            }
            swapTime = 0
            swapLock.unlock()
        }
    }

    @objc func fillVsync(info: UnsafeMutablePointer<vo_vsync_info>) {
        if option.mac.macos_render_timer != RENDER_TIMER_PRESENTATION_FEEDBACK { return }

        let next = presentation?.next()
        info.pointee.vsync_duration = next?.duration ?? -1
        info.pointee.skipped_vsyncs = next?.skipped ?? -1
        info.pointee.last_queue_display_time = next?.time ?? -1
    }

    @objc func isVisible() -> Bool {
        var visible = false
        let updateVisibility = {
            if let hostView = self.embeddedHostView {
                visible = !hostView.isHidden &&
                          (hostView.window?.occlusionState.contains(.visible) ?? false)
            } else {
                visible = self.window?.occlusionState.contains(.visible) ?? false
            }
        }

        if Thread.isMainThread {
            updateVisibility()
        } else {
            DispatchQueue.main.sync(execute: updateVisibility)
        }
        return visible || option.vo.force_render || needsInitialDraw
    }

    @objc func update(alpha: Bool) {
        layer?.wantsAlpha = alpha
        DispatchQueue.main.sync {
            window?.isOpaque = !alpha
            window?.backgroundColor = alpha ? NSColor.clear : nil
        }
    }

    override func displayLinkCallback(_ displayLink: CVDisplayLink,
                                      _ inNow: UnsafePointer<CVTimeStamp>,
                                      _ inOutputTime: UnsafePointer<CVTimeStamp>,
                                      _ flagsIn: CVOptionFlags,
                                      _ flagsOut: UnsafeMutablePointer<CVOptionFlags>) -> CVReturn {
        let signalSwap = {
            self.swapLock.lock()
            self.swapTime += 1
            self.swapLock.signal()
            self.swapLock.unlock()
        }

        if option.mac.macos_render_timer > RENDER_TIMER_SYSTEM {
            if let timer = self.timer, option.mac.macos_render_timer == RENDER_TIMER_PRECISE {
                timer.scheduleAt(time: inOutputTime.pointee.hostTime, closure: signalSwap)
                return kCVReturnSuccess
            }

            signalSwap()
            return kCVReturnSuccess
        }

        if option.mac.macos_render_timer == RENDER_TIMER_PRESENTATION_FEEDBACK {
            presentation?.add(time: inOutputTime.pointee)
        }

        return kCVReturnSuccess
    }

    override func startDisplayLink(_ vo: UnsafeMutablePointer<vo>) {
        super.startDisplayLink(vo)
        timer?.updatePolicy(periodSeconds: 1 / currentFps())
    }

    override func updateDisplaylink() {
        super.updateDisplaylink()
        timer?.updatePolicy(periodSeconds: 1 / currentFps())
    }

    override func updateICCProfile() {
        flagEvents(VO_EVENT_ICC_PROFILE_CHANGED)
    }

    override func windowDidResize() {
        flagEvents(VO_EVENT_RESIZE | VO_EVENT_EXPOSE)
    }

    override func windowDidChangeScreenProfile() {
        updateICCProfile()
    }

    override func windowDidChangeBackingProperties() {
        layer?.contentsScale = window?.backingScaleFactor ?? 1
        windowDidResize()
    }

    override func windowDidChangeOcclusionState() {
        flagEvents(VO_EVENT_EXPOSE)
    }
}
