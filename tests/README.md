hs test suite
=============

Overview
--------

A manifest-driven, golden-file regression suite for the hs shell. The
runner is a pure POSIX shell script that drives any POSIX shell against
the manifest, comparing each test's stdout / stderr / exit code against
sibling golden files.

The same runner exercises every build:

    ./build/hs-gcc  (gcc)
    ./build/hs-tcc  (tcc)
    ./build/hs      (M2-Planet)

and the host shell as an oracle:

    /bin/sh         (drift check against POSIX behavior)

There are no skip tags, no per-variant exceptions: every test runs
against every target. Behavioral parity is the contract.

Running the suite
-----------------

Each target shell drives the runner against itself:

    ./build/hs-gcc tests/run.sh --target ./build/hs-gcc
    ./build/hs-tcc tests/run.sh --target ./build/hs-tcc
    ./build/hs     tests/run.sh --target ./build/hs

Oracle (verifies hs hasn't drifted from POSIX behavior):

    /bin/sh tests/run.sh --target /bin/sh

Run from the project root.

Runner flags:

    --target PATH     binary under test (default ./build/hs-gcc)
    --filter SUBSTR   only tests whose manifest path contains SUBSTR
    --timeout SEC     per-test wall-clock limit (default 10s, 0 disables)
    --update          on mismatch, overwrite goldens with actual output
    --verbose, -v     print PASS lines (not only FAIL)

The runner applies a per-test timeout (default 10s) via the external
`timeout` command so a single hanging test cannot wedge the whole run.

Adding a test
-------------

1.  Create `tests/<category>/<name>.sh`. The test must be portable
    POSIX shell so that both hs and the host shell produce the same
    output. Anything hs-specific belongs nowhere in the suite.

2.  Append the test path to `tests/MANIFEST`.

3.  Bootstrap the goldens:

        ./build/hs-gcc tests/run.sh --filter <name> --update

4.  Inspect the generated `.out` / `.err` / `.exit` files. If they
    look wrong, fix the test or the expectations and rerun --update.

5.  Run the full suite + oracle to confirm nothing regressed:

        ./build/hs-gcc tests/run.sh --target ./build/hs-gcc
        /bin/sh tests/run.sh --target /bin/sh

Golden file conventions
-----------------------

For each `foo.sh` the runner reads up to three sibling files:

    foo.out    required; expected stdout (byte-exact)
    foo.err    optional; expected stderr (not checked if missing)
    foo.exit   optional; expected exit code as decimal (defaults to 0)

When --update is passed, the runner overwrites all three. A `.err`
file is only created when the test actually emits stderr; an empty
one is removed.
