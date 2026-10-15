#!/usr/bin/env sh
# Host-side simulation of the non-blocking USB send path + print queue
# against a scripted fake printer (stub/usb/usb_host.h). Needs only g++.
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -w -Istub -I../../src sim.cpp -Wl,--unresolved-symbols=ignore-all -o /tmp/stikka-usb-sim
/tmp/stikka-usb-sim
