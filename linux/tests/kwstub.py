#!/usr/bin/env python3
"""A stub org.kde.KWin /Scripting service for the Linux bar's behaviour test.

The bar's freeze fix has one observable contract on the bus: it may load the
KWin helper ONLY after unloading, and only when KWin says the script is not
already loaded. A source grep cannot prove that, so this stub claims the name
the real KWin owns, answers isScriptLoaded, and appends one line per call to a
log the test reads:

  isScriptLoaded <name>
  unloadScript <name>
  loadScript <path> <name>
  start

Usage: kwstub.py <bus-address> <log-file> <loaded-0|1> [report-address]
"""
import sys

import dbus
import dbus.service
import dbus.mainloop.glib
from gi.repository import GLib


class Scripting(dbus.service.Object):
    def __init__(self, bus, log, loaded):
        super().__init__(bus, "/Scripting")
        self.log = log
        self.loaded = loaded

    def _note(self, what):
        with open(self.log, "a") as f:
            f.write(what + "\n")

    @dbus.service.method("org.kde.kwin.Scripting", in_signature="s", out_signature="b")
    def isScriptLoaded(self, name):
        self._note("isScriptLoaded %s" % name)
        return bool(self.loaded)

    @dbus.service.method("org.kde.kwin.Scripting", in_signature="s", out_signature="")
    def unloadScript(self, name):
        self._note("unloadScript %s" % name)

    @dbus.service.method("org.kde.kwin.Scripting", in_signature="ss", out_signature="")
    def loadScript(self, path, name):
        self._note("loadScript %s %s" % (path, name))

    @dbus.service.method("org.kde.kwin.Scripting", in_signature="", out_signature="")
    def start(self):
        self._note("start")


def main():
    address, log, loaded = sys.argv[1], sys.argv[2], sys.argv[3]
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.bus.BusConnection(address)
    name = dbus.service.BusName("org.kde.KWin", bus)
    Scripting(bus, log, loaded == "1")
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
