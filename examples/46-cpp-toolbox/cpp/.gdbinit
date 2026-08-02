# Loads the Digest pretty-printer (toolbox-printers.py). Invoked from the
# example root (examples/46-cpp-toolbox/) as:
#   gdb -batch -x cpp/.gdbinit -ex 'b cmd_report' -ex run \
#       -ex 'p d' cpp/build/debug/toolbox report
set debuginfod enabled off
source cpp/toolbox-printers.py
