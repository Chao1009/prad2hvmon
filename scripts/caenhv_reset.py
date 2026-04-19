#!/usr/bin/env python3
# ─────────────────────────────────────────────────────────────────────────────
# caenhv_reset — remote reboot of CAEN HV mainframes via tsconnect
#
# Performs a soft (CPU) or hard (power cycle) reset of a CAEN SY1527/SY4527
# mainframe by driving the reset line through a terminal-server serial port.
#
# Requirements:
#   - $CLON_PARMS pointing to the CLON parameter directory
#   - $EPICS pointing to the EPICS tools tree
#   - SSH access to clon00 (runs the reset binary with sudo)
#   - tsconnect.conf mapping <mainframe>_reset to a /dev/tty device
#
# Original author: Nathan Baltzell — Jefferson Lab
# Modernised for Python 3 and integrated into prad2hvmon tools.
# ─────────────────────────────────────────────────────────────────────────────

import sys
import os
import argparse
import subprocess


def find_device(config_file: str, mainframe: str) -> str:
    """Look up the tty device for <mainframe>_reset in tsconnect.conf."""
    target = f"{mainframe}_reset"
    with open(config_file) as f:
        for line in f:
            if target in line:
                return line.strip().split()[0]
    return ""


def main():
    cli = argparse.ArgumentParser(
        description="CAEN HV Mainframe Reset (via tsconnect serial port)")
    mode = cli.add_mutually_exclusive_group(required=True)
    mode.add_argument(
        "--soft", action="store_true",
        help="CPU reboot only — should not affect voltages")
    mode.add_argument(
        "--hard", action="store_true",
        help="full power cycle — brings down ALL voltages")
    cli.add_argument(
        "mainframe",
        help="hostname of the mainframe (e.g. hallb-hv1)")

    args = cli.parse_args()
    opt = "--hard" if args.hard else "--soft"

    # ── Validate environment ──────────────────────────────────────────────
    clon_parms = os.environ.get("CLON_PARMS", "")
    epics = os.environ.get("EPICS", "")

    if not clon_parms or not os.path.isdir(clon_parms):
        sys.exit(f"$CLON_PARMS is not set or does not exist: '{clon_parms}'")

    config_file = os.path.join(clon_parms, "tsconnect", "tsconnect.conf")
    if not os.path.isfile(config_file):
        sys.exit(f"tsconnect config not found: {config_file}")

    exe = os.path.join(epics, "tools", "caenhvReset", "caenhvReset")
    if not os.path.isfile(exe):
        sys.exit(f"reset binary not found: {exe}")

    # ── Look up device ────────────────────────────────────────────────────
    device = find_device(config_file, args.mainframe)
    if not device:
        sys.exit(
            f"No device '{args.mainframe}_reset' in {config_file}")

    # ── Confirm hard reset ────────────────────────────────────────────────
    if args.hard:
        print(f"WARNING: hard reset of {args.mainframe} will power-cycle "
              f"the crate and bring down ALL voltages.")
        answer = input("Type 'yes' to confirm: ")
        if answer.strip().lower() != "yes":
            sys.exit("Aborted.")

    # ── Execute ───────────────────────────────────────────────────────────
    cmd = ["ssh", "clon00", "sudo", exe, opt, f"/dev/tty{device}"]
    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True)

    if result.stdout:
        print(result.stdout.rstrip())
    if result.stderr:
        print(result.stderr.rstrip(), file=sys.stderr)
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()
