# Ceph - a scalable distributed storage system

See https://ceph.com/ for current information about Ceph.


## Contributing Code

Most of Ceph is dual-licensed under the LGPL version 2.1 or 3.0. Some
miscellaneous code is either public domain or licensed under a BSD-style
license.

The Ceph documentation is licensed under Creative Commons Attribution Share
Alike 3.0 (CC-BY-SA-3.0). 

Some headers included in the `ceph/ceph` repository are licensed under the GPL.
See the file `COPYING` for a full inventory of licenses by file.

All code contributions must include a valid "Signed-off-by" line. See the file
`SubmittingPatches.rst` for details on this and instructions on how to generate
and submit patches.

Assignment of copyright is not required to contribute code. Code is
contributed under the terms of the applicable license.


## Checking out the source

Clone the ceph/ceph repository from github by running the following command on
a system that has git installed:

	git clone git@github.com:ceph/ceph

Alternatively, if you are not a github user, you should run the following
command on a system that has git installed:

	git clone git://github.com/ceph/ceph

When the `ceph/ceph` repository has been cloned to your system, run the
following commands to move into the cloned `ceph/ceph` repository and to check
out the git submodules associated with it:

    cd ceph
	git submodule update --init --recursive


## Build Prerequisites

*section last updated 27 Jul 2023*

Make sure that ``curl`` is installed. The Debian and Ubuntu ``apt`` command is
provided here, but if you use a system with a different package manager, then
you must use whatever command is the proper counterpart of this one:

    apt install curl

Install Debian or RPM package dependencies by running the following command:

	./install-deps.sh

Install the ``python3-routes`` package:

    apt install python3-routes


## Building Ceph

These instructions are meant for developers who are compiling the code for
development and testing. To build binaries that are suitable for installation
we recommend that you build `.deb` or `.rpm` packages, or refer to
``ceph.spec.in`` or ``debian/rules`` to see which configuration options are
specified for production builds.

To build Ceph, make sure that you are in the top-level `ceph` directory that
contains `do_cmake.sh` and `CONTRIBUTING.rst` and run the following commands:

	./do_cmake.sh
	cd build
	ninja

``do_cmake.sh`` by default creates a "debug build" of Ceph, which can be up to
five times slower than a non-debug build.  Pass
``-DCMAKE_BUILD_TYPE=RelWithDebInfo`` to ``do_cmake.sh`` to create a non-debug
build.

[Ninja](https://ninja-build.org/) is the buildsystem used by the Ceph project
to build test builds.  The number of jobs used by `ninja` is derived from the
number of CPU cores of the building host if unspecified. Use the `-j` option to
limit the job number if the build jobs are running out of memory. If you
attempt to run `ninja` and receive a message that reads `g++: fatal error:
Killed signal terminated program cc1plus`, then you have run out of memory.
Using the `-j` option with an argument appropriate to the hardware on which the
`ninja` command is run is expected to result in a successful build. For example,
to limit the job number to 3, run the command `ninja -j 3`. On average, each
`ninja` job run in parallel needs approximately 2.5 GiB of RAM.

This documentation assumes that your build directory is a subdirectory of the
`ceph.git` checkout. If the build directory is located elsewhere, point
`CEPH_GIT_DIR` to the correct path of the checkout. Additional CMake args can
be specified by setting ARGS before invoking ``do_cmake.sh``.  See [cmake
options](#cmake-options) for more details. For example:

    ARGS="-DCMAKE_C_COMPILER=gcc-7" ./do_cmake.sh

To build only certain targets, run a command of the following form:

	ninja [target name]

To install:

	ninja install
 
### CMake Options

If you run the `cmake` command by hand, there are many options you can
set with "-D". For example, the option to build the RADOS Gateway is
defaulted to ON. To build without the RADOS Gateway:

	cmake -DWITH_RADOSGW=OFF [path to top-level ceph directory]

Another example below is building with debugging and alternate locations 
for a couple of external dependencies:

	cmake -DLEVELDB_PREFIX="/opt/hyperleveldb" \
	-DCMAKE_INSTALL_PREFIX=/opt/ceph -DCMAKE_C_FLAGS="-Og -g3 -gdwarf-4" \
	..

To view an exhaustive list of -D options, you can invoke `cmake` with:

	cmake -LH

If you often pipe `ninja` to `less` and would like to maintain the
diagnostic colors for errors and warnings (and if your compiler
supports it), you can invoke `cmake` with:

	cmake -DDIAGNOSTICS_COLOR=always ...

Then you'll get the diagnostic colors when you execute:

	ninja | less -R

Other available values for 'DIAGNOSTICS_COLOR' are 'auto' (default) and
'never'.


## Building a source tarball

To build a complete source tarball with everything needed to build from
source and/or build a (deb or rpm) package, run

	./make-dist

This will create a tarball like ceph-$version.tar.bz2 from git.
(Ensure that any changes you want to include in your working directory
are committed to git.)


## Running a test cluster

From the `ceph/` directory, run the following commands to launch a test Ceph
cluster:

	cd build
	ninja vstart        # builds just enough to run vstart
	../src/vstart.sh --debug --new -x --localhost --bluestore
	./bin/ceph -s

Most Ceph commands are available in the `bin/` directory. For example:

	./bin/rbd create foo --size 1000
	./bin/rados -p foo bench 30 write

To shut down the test cluster, run the following command from the `build/`
directory:

	../src/stop.sh

Use the sysvinit script to start or stop individual daemons: 

	./bin/init-ceph restart osd.0
	./bin/init-ceph stop


## Running unit tests

To build and run all tests (in parallel using all processors), use `ctest`:

	cd build
	ninja
	ctest -j$(nproc)

(Note: Many targets built from src/test are not run using `ctest`.
Targets starting with "unittest" are run in `ninja check` and thus can
be run with `ctest`. Targets starting with "ceph_test" can not, and should
be run by hand.)

When failures occur, look in build/Testing/Temporary for logs.

To build and run all tests and their dependencies without other
unnecessary targets in Ceph:

	cd build
	ninja check -j$(nproc)

To run an individual test manually, run `ctest` with -R (regex matching):

	ctest -R [regex matching test name(s)]

(Note: `ctest` does not build the test it's running or the dependencies needed
to run it)

To run an individual test manually and see all the tests output, run
`ctest` with the -V (verbose) flag:

	ctest -V -R [regex matching test name(s)]

To run tests manually and run the jobs in parallel, run `ctest` with 
the `-j` flag:

	ctest -j [number of jobs]

There are many other flags you can give `ctest` for better control
over manual test execution. To view these options run:

	man ctest


## Building the Documentation

### Prerequisites

The list of package dependencies for building the documentation can be
found in `doc_deps.deb.txt`:

	sudo apt-get install `cat doc_deps.deb.txt`

### Building the Documentation

To build the documentation, ensure that you are in the top-level
`/ceph` directory, and execute the build script. For example:

	admin/build-doc

## Reporting Issues

To report an issue and view existing issues, please visit https://tracker.ceph.com/projects/ceph.

# Test Twotone

## 虚拟环境搭建
```
cd build ../src/stop.sh; rm -rf out/ dev/

MDS=0 MON=1 MGR=1 OSD=6 ../src/vstart.sh -n -x --without-dashboard --filestore

export PYTHONPATH=/root/ceph/src/pybind:/root/ceph/build/lib/cython_modules/lib.3:/root/ceph/src/python-common:$PYTHONPATH
export LD_LIBRARY_PATH=/root/ceph/build/lib:$LD_LIBRARY_PATH
export PATH=/root/ceph/build/bin:$PATH
alias cephfs-shell=/root/ceph/src/tools/cephfs/cephfs-shell
CEPH_DEV=1
```

## Twotone测试

**Delete the stuck pool** (if not already done):
`./bin/ceph osd pool rm ec-twotone-small ec-twotone-small --yes-i-really-really-mean-it`

**Recreate the erasure code profile with crush-failure-domain=osd:**
```
./bin/ceph osd erasure-code-profile rm twotone-small   # Remove old if exists
./bin/ceph osd erasure-code-profile set twotone-small plugin=twotone k=2 m=2 crush-failure-domain=osd directory=$PWD/lib --force
```

**Recreate the pool:**
`./bin/ceph osd pool create ec-twotone-small 64 64 erasure twotone-small`

**Set min_size to 2** (as before, to ensure activation):
`./bin/ceph osd pool set ec-twotone-small min_size 2`

**Optional: Enable application tag** (silences a minor warning): `./bin/ceph osd pool application enable ec-twotone-small rados`

**Monitor progress** (PGs should now activate):
```
watch ./bin/ceph -s
./bin/ceph pg ls-by-pool ec-twotone-small
./bin/ceph health detail

./bin/ceph config set osd debug_osd 20
./bin/ceph config set osd debug_filestore 10
tail -f out/osd.*.log | tee logk2m1.txt
```

Once PGs are active (no more "inactive/incomplete" warnings for this pool)

**Write to PG**
```
./bin/rados -c ceph.conf -k keyring -p ec-twotone-small put abobj ./test/abobj.txt
```

**Read from PG**
```
./bin/rados -c ceph.conf -k keyring -p ec-twotone-small get abobj ./test/recovered_abobj.txt
```

**Check OSD Map**
```
root@sealion:~/ceph/build# ./bin/ceph osd map ec-twotone-small test_object

osdmap e38 pool 'ec-twotone-small' (1) object 'test_object' -> pg 1.8416ffa4 (1.24) -> up ([3,0,4], p3) acting ([3,0,4], p3)
root@sealion:~/ceph/build# ./bin/ceph osd map ec-twotone-small testobj --format json-pretty

{
    "epoch": 38,
    "pool": "ec-twotone-small",
    "pool_id": 1,
    "objname": "test_object",
    "raw_pgid": "1.8416ffa4",
    "pgid": "1.24",
    "up": [
        3,
        0,
        4
    ],
    "up_primary": 3,
    "acting": [
        3,
        0,
        4
    ],
    "acting_primary": 3
}
```
Bigger file, vstart with --filestore instead of bluestore to better spot files
```
root@sealion:~/ceph/build# hexdump -C test/abobj.txt 
00000000  41 41 41 41 41 41 41 41  41 41 41 41 41 41 41 41  |AAAAAAAAAAAAAAAA|
*
00001000  42 42 42 42 42 42 42 42  42 42 42 42 42 42 42 42  |BBBBBBBBBBBBBBBB|
*
00002000
root@sealion:~/ceph/build# ./bin/rados -c ceph.conf -k keyring -p ec-twotone-small put abobj ./test/abobj.txt 

root@sealion:~/ceph/build# ./bin/ceph osd map ec-twotone-small abobj --format json-pretty
{
    "epoch": 30,
    "pool": "ec-twotone-small",
    "pool_id": 1,
    "objname": "abobj",
    "raw_pgid": "1.c192046b",
    "pgid": "1.2b",
    "up": [
        3,
        2,
        0
    ],
    "up_primary": 3,
    "acting": [
        3,
        2,
        0
    ],
    "acting_primary": 3
}
root@sealion:~/ceph/build# hexdump -C dev/osd3/current/1.2bs0_head/abobj__head_C192046B__1_ffffffffffffffff_0
00000000  41 41 41 41 41 41 41 41  41 41 41 41 41 41 41 41  |AAAAAAAAAAAAAAAA|
*
00001000
root@sealion:~/ceph/build# hexdump -C dev/osd2/current/1.2bs1_head/abobj__head_C192046B__1_ffffffffffffffff_1 
00000000  42 42 42 42 42 42 42 42  42 42 42 42 42 42 42 42  |BBBBBBBBBBBBBBBB|
*
00001000
root@sealion:~/ceph/build# hexdump -C dev/osd0/current/1.2bs2_head/abobj__head_C192046B__1_ffffffffffffffff_2 
00000000  03 03 03 03 03 03 03 03  03 03 03 03 03 03 03 03  |................|
*
00001000
```