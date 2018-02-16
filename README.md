
SparseFS
--------

FUSE client that creates a sparse view of one or more directories

Usage
-----

```
usage: ./sparsefs [options] mountpoint

general options:
    -o opt,[opt...]        mount options
    -h   --help            print help
    -V   --version         print version

FilterFS options:
    -s <source dir>                        source directory
    -X, --exclude=<pattern>[:<pattern>...] patterns for files to be excluded
    -I, --include=<pattern>[:<pattern>...] patterns for files to be included
    --excludefile=<filename>               file with one exclude pattern in each line
    --includefile=<filename>               file with one include pattern in each line
    --default-exclude                      exclude unmatched items (default)
    --default-include                      include unmatched items
```

Include and exclude filters are specified on the command line or alternatively
in the system's fstab file with the following parameters:

```
  -I <pattern> OR --include=<pattern> OR -oinclude=<pattern>

  -X <pattern> OR --exclude=<pattern> OR -oexclude=<pattern>
```

The -o syntax takes the form of a mount option, which is needed to specify these
in the system fstab. In place of any of the <pattern> placeholders above
multiple patterns separated by colons can be used, yielding e.g.:

```
  -I <pattern>[:<pattern>[...]] OR --exclude=<pattern>[:<pattern>[...]]
```

Additionally, these patterns can be specified in a file with one pattern per
line. Such a file can be passed to SparseFS with

```
  --includefile=<filename> OR --excludefile=<filename>
```

The default action for a SparseFS filesystem is to include all files that cannot
be matched by any rule. It is also possible to override this default behaviour
and exclude all unmatched files with the following parameter:

```
  --default-exclude
```

The order that rules are provided on the command line is the same as their order
in the filter chain.

SparseFS requires at least one source directory. If multiple source directories
were specified, the files and directories of the sources are merged into one
hierarchy. If a file exists in multiple sources, the file from the first source
is shown. If you need more control over how the hierarchies are merged, see
[MergerFS](https://github.com/trapexit/mergerfs) for a more mature solution.

Note that mounting SparseFS filesystems with the fstab file requires the
/sbin/mount.fuse utility from the fuse-utils package.

Building
--------

SparseFS only depends on the FUSE implementation. Make sure you have the
necessary files installed, e.g., by installing `libfuse-dev` on Debian-based
systems.

To build SparseFS, execute:

```
aclocal
autoconf
automake --add-missing
make
```

License
-------

SparseFS can be distributed under the terms of the GNU GPL version 3 or
later. You can find it [online](http://www.gnu.org/licenses/gpl-3.0.html) or
in the COPYING file.

The included wildmatch implementation is licensed under the Apache License,
Version 2.0. Copyright 1986-present, Rich $alz, Wayne Davison, and Duy Nguyen.

SparseFS is based on [FilterFs](http://filterfs.sourceforge.net) by Gregor
Zurowski <gregor.zurowski@lunetta.net> and Kristofer Henriksson
<kthenriksson@gmail.com> and includes additional patches by Eddy Petrișor
<eddy.petrisor@gmail.com>.
