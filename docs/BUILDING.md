# Building MergeDesk

The canonical automated build is
[`.github/workflows/build.yml`](../.github/workflows/build.yml). It builds
Release binaries with native MSVC on Windows and GCC on GNU/Linux. The source
uses C++17 and Qt 6 Widgets, Concurrent, WebEngineWidgets and Svg; Qt Linguist
tools compile the translation catalogs. QtDBus is used on GNU/Linux.

## GNU/Linux

Debian 12 is the release package baseline (glibc 2.36). Ubuntu 24.04 is also
used for development and regression checks. Use the reviewed **Qt 6.11.3**
Linux x86-64 SDK, including WebEngine, WebChannel, Positioning, ImageFormats,
Wayland, translations, and the GTK/desktop-portal plugins. The old Qt 6.4 SDK
from these distributions is not suitable for this application.

Install the native development dependencies and runtime libraries. Package
names below are for Debian 12; Ubuntu 24.04 uses `libgtk-3-0t64` and
`libasound2t64` in place of their corresponding unsuffixed names.

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build python3 python3-venv \
  pkg-config coreutils libpoco-dev libicu-dev libboost-dev libarchive-dev \
  libgtest-dev re2c lemon xvfb xauth patchelf squashfs-tools file \
  fonts-dejavu-core dbus python3-dbus python3-gi libgtk-3-0 libnss3 libasound2 \
  libgl1-mesa-dev libegl1-mesa-dev libvulkan-dev libxkbcommon-x11-0 \
  libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0 \
  libxcb-shape0 libxcb-xinerama0 libxcomposite1 libxdamage1 libxrandr2 \
  libxtst6 libxshmfence1 libgbm1 libinput10

python3 -m venv .qt-installer
.qt-installer/bin/python -m pip install aqtinstall==3.3.0
.qt-installer/bin/python -m aqt install-qt linux desktop 6.11.3 linux_gcc_64 \
  -O "$PWD/Qt" -m qtimageformats qtwebchannel qtpositioning qtwebengine
export QT_ROOT_DIR="$PWD/Qt/6.11.3/gcc_64"
export PATH="$QT_ROOT_DIR/bin:$PATH"
export CMAKE_PREFIX_PATH="$QT_ROOT_DIR"
```

The SDK version must match [`packaging/qt-runtime.json`](../packaging/qt-runtime.json)
when creating release packages. The native workflow installs the same SDK and
verifies the actual desktop plugins and relocated WebEngine helper.

Configure and compile from the repository root:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure --parallel 3
packaging/run_selftests.sh "$PWD/build/app/mergedesk"
QT_QPA_PLATFORM=offscreen build/app/mergedesk --selftest-safety
QT_QPA_PLATFORM=offscreen build/app/mergedesk --selftest-mergedesk-features
QT_QPA_PLATFORM=xcb QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu \
  QT_QUICK_BACKEND=software xvfb-run -a build/app/mergedesk --selftest-web
/usr/bin/python3 packaging/linux/verify_native_dialogs.py \
  --executable build/app/mergedesk --output build/NATIVE-DIALOGS.json
```

The regression runner uses GNU coreutils `timeout` to bound each application
invocation. It also recognizes `gtimeout` where GNU tools use that prefix.

Run `build/app/mergedesk` in your normal desktop session. The headless settings
above are for tests. The application does not disable Chromium's sandbox;
the Debian CI container sets a test-only override because that isolated
container runs as root.

The native-dialog fixture starts a private D-Bus session and uses the current
X display or starts Xvfb. It exercises actual Qt file choosers for KDE/GNOME
desktop identification and checks open/save/folder requests, returned paths,
loaded portal plugins and explicit startup overrides. For a relocated package,
add `--expected-plugin-root /absolute/package/plugins`; for an installed DEB,
use `/usr/lib/mergedesk/plugins`. The real desktop needs `xdg-desktop-portal`
and its KDE/GNOME FileChooser backend for portal-native dialogs; the fixture
supplies only an isolated test service and never replaces the user's portal.

For release packaging, build on the baseline distribution and configure its
private runtime layout explicitly:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib
cmake --build build --parallel 3
python3 packaging/linux/package.py --build-dir build --output-dir dist \
  --qt-root "$QT_ROOT_DIR" --require-debian12
```

Packaging creates a portable TAR.GZ, a DEB and a type-2 AppImage. It collects
runtime dependencies and their license/source records, sets relative runtime
paths, and runs the relocated application with development-library paths
removed. AppImage build tools and runtime downloads are checksum-pinned.
Enable the distribution's source-package repositories so exact corresponding
source URLs can be recorded. The workflow shows the Debian setup.

## Windows x64

Use Visual Studio 2022 C++ build tools, CMake, Ninja, Git and a Qt 6.11.3 MSVC
2022 x64 SDK containing WebEngine, WebChannel, Positioning and ImageFormats.
Run the commands in a Developer PowerShell with the x64 compiler environment.
The workflow pins an upstream `aqtinstall` commit containing its Qt 6.11 Windows
repository-layout fix; the older 3.3.0 Windows installer cannot read that layout.
The native workflow records the exact installer and SDK pins.
MergeDesk requires Windows 10 version 1903 or later, or Windows 11. Its
manifest selects the UTF-8 process code page, supported beginning in
[Windows 10 1903](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page).
This is slightly newer than Qt 6.11's own
[Windows 10 1809 baseline](https://doc.qt.io/qt-6.11/supported-platforms.html).

Set the path to your actual Qt installation, then obtain the exact vcpkg
revision used by the project:

```powershell
$qtSdkDirectory = 'C:/Qt/6.11.3/msvc2022_64'
git clone https://github.com/microsoft/vcpkg.git .vcpkg
git -C .vcpkg checkout 351757dd7b0c6518412ca98b3ef7982861a47fc6
.\.vcpkg\bootstrap-vcpkg.bat -disableMetrics

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_PREFIX_PATH=$qtSdkDirectory" `
  "-DCMAKE_TOOLCHAIN_FILE=$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake" `
  "-DVCPKG_MANIFEST_DIR=$PWD/cmake/windows" `
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure --parallel 3
```

The manifest installs Poco Foundation, ICU, Boost headers, libarchive and
GoogleTest. The automated workflow saves their binary cache immediately after
installation, before configuring the application. Parser input hashes are
normalized for checkout line endings; semantic changes still require
regenerating the parser.

Make the Qt tools available in PATH for runtime tests and deployment, then run:

```powershell
$env:PATH = "$qtSdkDirectory/bin;$env:PATH"
./packaging/windows/run-selftests.ps1 -Executable build/app/MergeDesk.exe -Web
./packaging/windows/package.ps1 -BuildDirectory build -OutputDirectory dist
```

The portable ZIP contains `MergeDesk.exe` at its root, Qt/WebEngine resources,
plugins, native dependencies, required runtime files, translations and notices.
Packaging tests a relocated copy with development Qt/vcpkg directories removed
from PATH. It creates no installer. Preferences use the current user's profile;
extracting a portable program does not move preferences into its directory.

## Source and generated files

Tree-sitter grammar archives are pinned and checked before extraction. The
generated filter lexer/parser is included so Windows builds do not need a
separate re2c/lemon installation. To verify or refresh it on a machine with
those generators installed:

```sh
python3 packaging/regenerate_filter_parser.py --check
python3 packaging/regenerate_filter_parser.py
```

See [LOCALIZATION.md](LOCALIZATION.md) for catalog updates, and
[VALIDATION.md](VALIDATION.md) for test coverage, package checks and measurement
scope. See [RELEASING.md](RELEASING.md) for promotion of tested packages to a
versioned GitHub release.
Retain the licenses, original source notices and build scripts when
redistributing binaries or source archives.
