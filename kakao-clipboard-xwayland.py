#!/usr/bin/python3
"""Offer a Windows bitmap alongside a PNG-only GNOME clipboard image."""

from concurrent.futures import ThreadPoolExecutor
import logging
import os
import signal
import subprocess
import time

import gi

os.environ['GDK_BACKEND'] = 'x11'
gi.require_version('Gdk', '4.0')
gi.require_version('Gtk', '4.0')
from gi.repository import Gdk, Gio, GLib, Gtk

PNG = 'image/png'
BMP = 'image/bmp'
MAX_PNG_BYTES = 64 * 1024 * 1024


class ClipboardBridge:
    def __init__(self, clipboard):
        self.clipboard = clipboard
        self.loop = GLib.MainLoop()
        self.worker = ThreadPoolExecutor(max_workers=1)
        self.request = None
        self.job = None
        self.listener = clipboard.connect('changed', self.changed)
        GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, self.stop)
        GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGINT, self.stop)

    def cancel(self):
        if self.request is not None:
            self.request.cancel()
            self.request = None
        if self.job is not None:
            self.job.cancel()
            self.job = None

    def changed(self, clipboard):
        self.cancel()
        if clipboard.is_local() or set(clipboard.get_formats().get_mime_types()) != {PNG}:
            return
        self.request = Gio.Cancellable()
        clipboard.read_async([PNG], GLib.PRIORITY_DEFAULT, self.request,
                             self.read_ready, self.request)

    def read_ready(self, clipboard, result, request):
        try:
            stream, _ = clipboard.read_finish(result)
        except GLib.Error as error:
            if not request.is_cancelled():
                logging.warning('Could not read PNG from clipboard: %s', error.message)
            return
        if request.is_cancelled():
            stream.close(None)
            return
        self.job = self.worker.submit(self.convert, stream, request)

    def convert(self, stream, request):
        started = time.monotonic()
        try:
            png = bytearray()
            try:
                while chunk := stream.read_bytes(256 * 1024, request).get_data():
                    png.extend(chunk)
                    if len(png) > MAX_PNG_BYTES:
                        raise ValueError('Clipboard PNG exceeds size limit')
            finally:
                stream.close(None)
            if request.is_cancelled():
                return
            bmp = subprocess.run(
                ['/usr/bin/magick', '-limit', 'memory', '256MiB', '-limit',
                 'map', '512MiB', '-limit', 'disk', '512MiB', 'png:-',
                 '-background', 'white', '-alpha', 'remove', '-alpha', 'off',
                 '-type', 'TrueColor', 'BMP3:-'],
                input=png, capture_output=True, check=True, timeout=15,
            ).stdout
            if len(bmp) < 54 or bmp[:2] != b'BM' or bmp[14:18] != b'\x28\x00\x00\x00':
                raise ValueError('Unexpected BMP output')
            if not request.is_cancelled():
                providers = [
                    Gdk.ContentProvider.new_for_bytes(BMP, GLib.Bytes.new(bmp)),
                    Gdk.ContentProvider.new_for_bytes(PNG, GLib.Bytes.new(bytes(png))),
                ]
                provider = Gdk.ContentProvider.new_union(providers)
                GLib.idle_add(self.publish, request, provider, started)
        except (GLib.Error, OSError, ValueError, subprocess.SubprocessError) as error:
            if not request.is_cancelled():
                logging.warning('Clipboard conversion failed: %s', type(error).__name__)

    def publish(self, request, provider, started):
        if request is self.request and not request.is_cancelled():
            if self.clipboard.set_content(provider):
                logging.info('Offered PNG and BMP (%d ms)', (time.monotonic() - started) * 1000)
            else:
                logging.warning('Could not publish the converted clipboard image')
        return GLib.SOURCE_REMOVE

    def stop(self):
        self.clipboard.disconnect(self.listener)
        self.cancel()
        self.worker.shutdown(wait=False, cancel_futures=True)
        self.loop.quit()
        return GLib.SOURCE_REMOVE


if __name__ == '__main__':
    logging.basicConfig(level=logging.INFO, format='%(levelname)s %(message)s')
    Gtk.init()
    display = Gdk.Display.get_default()
    if display is None:
        raise SystemExit('XWayland display is unavailable')
    bridge = ClipboardBridge(display.get_clipboard())
    logging.info('Listening for PNG clipboard changes on XWayland')
    bridge.loop.run()
