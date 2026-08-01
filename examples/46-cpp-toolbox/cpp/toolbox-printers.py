"""gdb pretty-printer for toolbox's Digest type (ch46 "gdb printers" section).

Digest is `struct Digest { std::uint64_t fnv; };` -- without this printer gdb
shows the raw aggregate (`{fnv = 12345...}`); with it, `print`/`p` renders
`Digest(0x<16 lowercase hex digits>)`, matching the digest format `toolbox
report` itself prints.
"""

import gdb


class DigestPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        fnv = int(self.val["fnv"]) & 0xFFFFFFFFFFFFFFFF
        return "Digest(0x{:016x})".format(fnv)


def _lookup(val):
    t = val.type.unqualified().strip_typedefs()
    if t.tag == "Digest":
        return DigestPrinter(val)
    return None


def register_printers():
    gdb.pretty_printers.append(_lookup)


# `gdb source cpp/toolbox-printers.py` execs this file directly (gdb's
# Python "source" is not the module import system, so the hyphen in this
# filename is fine) -- register on load rather than requiring a separate
# `import`.
register_printers()
