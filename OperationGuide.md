= PRad-II HV Monitor — Shift Operator Guide =

The HV monitor daemon runs on '''clonpc19''' (port 8765). There are two ways to access it:

== Accessing the Monitor ==

=== Web GUI ===

Open Firefox and navigate to:

 http://clonpc19:8765/

=== Qt GUI ===

From a terminal:

 ssh clonpc19
 cd prad2_daq/prad2hvmon
 ./build/bin/prad2hvmon

If you cannot connect to the monitor, contact the '''run coordinator'''.

== Recovering Tripped Channels ==

If you see HyCal or LMS channels that have tripped off (red in the geometry view or status column), you can power them back on:

# Click the access pill in the header and log in with password <code>prad2_user</code> at the '''User''' level.
# Select the affected channels and turn them ON.
# '''Record every power-cycle operation on [https://logbooks.jlab.org/book/pradlog PRADLOG]''' — note which channels were recovered, when, and any relevant context (e.g., beam trip, temperature spike).

== Persistent Faults ==

If a channel keeps tripping repeatedly or shows a persistent fault that does not clear after power-cycling:

# '''Contact the run coordinator.'''
# Take screenshots of:
#* The '''Fault Log''' tab — showing the fault history for the affected channel.
#* The '''HyCal Geometry''' view with the color mode set to '''Vmon − Vset''' — to show which channels are deviating.
# Log the screenshots on [https://logbooks.jlab.org/book/pradlog PRADLOG].

== Taking Screenshots ==

=== Qt GUI (Ctrl+S) ===

Press '''Ctrl+S''' in the Qt GUI window. A timestamped PNG is saved automatically to <code>database/screenshots/</code> (filename format: <code>prad2hvmon_YYYYMMDD_HHmmss.png</code>). A confirmation message prints to the terminal.

=== RHEL9 Desktop (GNOME) ===

{| class="wikitable"
! Capture !! Shortcut
|-
| Full screen || <code>Print Screen</code>
|-
| Current window || <code>Alt + Print Screen</code>
|-
| Select a region || <code>Shift + Print Screen</code>, then click and drag
|}

Screenshots are saved to <code>~/Pictures/</code>. You can also run <code>gnome-screenshot --interactive</code> from a terminal for more options.
