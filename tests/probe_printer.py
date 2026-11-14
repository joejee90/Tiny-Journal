#!/usr/bin/env python3
"""Connect-only printer reachability probe. Sends no bytes and prints nothing."""
import argparse
import socket
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('host')
p.add_argument('--port', type=int, default=9100)
a = p.parse_args()
with socket.create_connection((a.host, a.port), timeout=3):
    print('TCP connection succeeded; printer protocol and physical printing are unverified.')
