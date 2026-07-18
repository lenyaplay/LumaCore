# native/cmake/

Toolchain files vendored here on Этап 2/3 (Task.md §6):

- `android.toolchain.cmake` — comes with the Android NDK, referenced via
  `-DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake`.
- `ios.toolchain.cmake` — [leetal/ios-cmake](https://github.com/leetal/ios-cmake),
  vendored as a submodule or a pinned single file.

Not vendored yet — the skeleton only needs the host (no-toolchain) configure
path to build `lumacore_logic` + `lumacore_tests`.
