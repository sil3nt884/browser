# Copyright 2026 The Umbra Authors.
#
# Loaded by every Python build script (env.sh puts this folder on
# PYTHONPATH). A container sees all of the host's CPUs even when it may only
# use a few, and some of Chromium's generators start one worker per CPU:
# Blink's binding generator then runs 24 workers that each load the whole
# Web IDL database and blow through the container's memory cap.
# CHORIUM_CPU_COUNT makes os.cpu_count() (and multiprocessing.cpu_count(),
# which calls it) report the number we choose instead.
import os

_count = os.environ.get("CHORIUM_CPU_COUNT")
if _count:
    _n = max(1, int(_count))
    os.cpu_count = lambda: _n
