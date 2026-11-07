The AppImage runtime comes from AppImage/type2-runtime at commit
fedfad51acc2860b2916a1eef32ba4a500ff31ef. The included source archive
contains the runtime source, build instructions, and the libfuse patch used
by the upstream build. The exact libfuse 3.15.0 source archive is also included
so the statically linked LGPL component can be modified and the runtime rebuilt.
Other permissively licensed static runtime dependencies have their full terms
in this directory. Alpine 3.21 package dependency versions are selected by the
upstream build scripts; the source manifest records the notice sources.

To rebuild, unpack type2-runtime-source.tar.gz and follow BUILD.md. The scripts
under scripts/ build libfuse and squashfuse, then link the runtime. Rebuilt
runtimes may be supplied to appimagetool with --runtime-file when repackaging
the unchanged MergeDesk AppDir. See packaging/linux/package.py in MergeDesk.
