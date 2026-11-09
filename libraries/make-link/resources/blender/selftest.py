#
#  selftest.py
#  libraries/make-link/resources/blender
#
#  Make's Connect self-test on Windows, run by Blender inside Make's box (MakeBox): one bare TCP connection to this
#  computer's own address and port, closed at once, nothing sent or read, and the outcome written into the result file
#  in the work folder: "refused" when the box kept the connection from going out (the firewall's rules for the box
#  hold), "reached" when it got through to the network stack (connected, or answered with a reset). Make offers its
#  Blender tools only on "refused". The app made the same connection unboxed first, as the control.
#    blender --background --factory-startup --disable-autoexec -noaudio --python selftest.py -- <address> <port> <result file>
#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#

import socket, sys

address, port, result = sys.argv[sys.argv.index("--") + 1:][:3]
outcome = "reached"
try:
    with socket.create_connection((address, int(port)), timeout=5):
        pass
except ConnectionRefusedError:
    outcome = "reached"   # a reset: the attempt got through to the network stack
except OSError:
    outcome = "refused"   # access denied, timed out, unreachable: the box held
with open(result, "w") as f:
    f.write(outcome + "\n")
