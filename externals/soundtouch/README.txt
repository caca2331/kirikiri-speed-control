Place prebuilt SoundTouch binaries here for offline builds.

Expected layout:
externals/soundtouch/
  include/soundtouch/SoundTouch.h   (and other headers)
  lib/x86/SoundTouch.lib
  lib/x64/SoundTouch.lib
  bin/x86/SoundTouch.dll
  bin/x64/SoundTouch.dll

The CMake build will prefer these vendored binaries; if they are missing, it will fall back to vcpkg (if available).
