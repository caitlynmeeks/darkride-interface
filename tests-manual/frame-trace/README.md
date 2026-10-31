# The frame trace (instruments plan, item 1)

One switch, one CSV. `Test.frameTrace(true)` starts a ring buffer of the last frames (2048, about 14 s at 144 Hz);
every subsystem that has an instrument writes its named numbers into the current frame's record while it is on and
pays one atomic check when it is off. `Test.dumpFrameTrace(path)` writes the CSV (a column per name, blank where a
frame has no value); `Test.takeFrameTrace()` returns `[columns, row, row, ...]` and clears the records. Developer >
Show Statistics shows the columns live ("Frame trace" box) while the trace is on. Developer > Recording Mode (item 9)
flips it with Stats for a recording.

The test: `frame-trace.js` with `--testScript ... quitWhenFinished`; the log says `FT PASS` when the dump has the
frame's own columns (`frame.dt_ms`, `frame.idle_ms`) and about three seconds of rows. The unit test is
tests/shared FrameTraceTests (the ring, the columns, the CSV, the Stats lines).

Columns are named `<area>.<name>_<unit>`, lowercase; the list of the planned ones is in
team/performance/INSTRUMENTS-PLAN.md ("The frame trace API").
