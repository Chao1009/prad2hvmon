# PRad-II HV Monitor — Shift Operator Guide

The HV monitor daemon runs on **clonpc19** (port 8765). There are two ways to access it:

## Accessing the Monitor

### Web GUI

Open Firefox and navigate to:

```
http://clonpc19:8765/
```

### Qt GUI

From a terminal:

```bash
ssh clonpc19
cd prad2_daq/prad2hvmon
./build/bin/prad2hvmon
```

If you cannot connect to the monitor, contact the **run coordinator**.

## Recovering Tripped Channels

If you see HyCal or LMS channels that have tripped off (red in the geometry view or status column), you can power them back on:

1. **Stop the DAQ first** before performing any power-cycle operations.
2. Click the access pill in the header and log in with password `prad2_user` at the **User** level.
3. Select the affected channels and turn them ON.
4. **Record every power-cycle operation on [PRADLOG](https://logbooks.jlab.org/book/pradlog)** — note which channels were recovered, when, and any relevant context (e.g., beam trip, temperature spike).

## Persistent Faults

If a channel keeps tripping repeatedly or shows a persistent fault that does not clear after power-cycling:

1. **Contact the run coordinator.**
2. Take screenshots of:
   - The **Fault Log** tab — showing the fault history for the affected channel.
   - The **HyCal Geometry** view with the color mode set to **Vmon − Vset** — to show which channels are deviating.
3. Log the screenshots on [PRADLOG](https://logbooks.jlab.org/book/pradlog).

## Taking Screenshots

### Qt GUI (Ctrl+S)

Press **Ctrl+S** in the Qt GUI window. A timestamped PNG is saved automatically to `database/screenshots/` (filename format: `prad2hvmon_YYYYMMDD_HHmmss.png`). A confirmation message prints to the terminal.

### RHEL9 Desktop (GNOME)

| Capture | Shortcut |
|---------|----------|
| Full screen | `Print Screen` |
| Current window | `Alt + Print Screen` |
| Select a region | `Shift + Print Screen`, then click and drag |

Screenshots are saved to `~/`. You can also run `gnome-screenshot --interactive` from a terminal for more options.
