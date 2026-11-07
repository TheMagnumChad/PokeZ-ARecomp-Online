"""Platform names, compiler options and advisory locking for Lumiose."""
import os
import sys

WINDOWS = os.name == 'nt'
MACOS = sys.platform == 'darwin'
EXECUTABLE = 'lasrecomp.exe' if WINDOWS else 'lasrecomp'
LIBRARY_SUFFIX = '.dll' if WINDOWS else '.dylib' if MACOS else '.so'
GAME_LIBRARY = 'lasgame' + LIBRARY_SUFFIX
MODULE_LIBRARY = 'lumiose' + LIBRARY_SUFFIX
ZIG = 'zig.exe' if WINDOWS else 'zig'
POSIX_TARGET = 'aarch64-macos.15.0' if MACOS else 'x86_64-linux-gnu'
POSIX_COMPILE = ['-fPIC', '-DGUEST_VOLATILE', '-target', POSIX_TARGET]
POSIX_LINK = ['-target', POSIX_TARGET, '-shared']
if MACOS:
    POSIX_LINK.append('-Wl,-undefined,dynamic_lookup')

def lock_file(file):
    if WINDOWS:
        import msvcrt
        file.seek(0)
        if not file.read(1):
            file.write(b'0')
            file.flush()
        file.seek(0)
        msvcrt.locking(file.fileno(), msvcrt.LK_NBLCK, 1)
    else:
        import fcntl
        fcntl.flock(file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
