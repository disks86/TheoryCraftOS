# App Manager

The App Manager is the desktop's package manager for AppImages. Open it from the Start menu ("App Manager"). It is built on the catalog of
[AppMan](https://github.com/ivan-hc/AM) (the `am` / `appman` tool), which lists about 9,000 AppImages and has an install script for each one.

- **Search** narrows the list by name and description as you type. Names that start with the search come first.
- **Show** switches between all AppImages and the installed ones.
- **Install**, **Update** and **Remove** act on the chosen application. Installing also happens on a double click. **Cancel** stops a
  download.
- **Refresh list** fetches AppMan's catalog again. A copy is kept in `/AppStore/AppMan/stats-appimages`, so the list is there at once next
  time.

An installed program appears in the Start menu as "Name (Linux)" and runs like any AppImage (see AppImage.md): mounted privately, with its
AppRun worked out, and Electron programs get `--no-sandbox`.

## How a program is installed

`desktop::appman::Installer` (src/Desktop/AppMan/AppManInstaller.hpp) does what AppMan's own scripts do, with the same layout in the Linux
world, so AppMan or a person finds everything where they expect it.

1. **Fetch the install script** from `programs/x86_64/<name>` of the AppMan repository.
2. **Find the download link without running the script.** AppMan's scripts find the current release with one pipeline, such as:

   ```sh
   version=$(curl -Ls https://api.github.com/repos/<owner>/<repo>/releases | sed 's/[()",{} ]/\n/g' | grep -oi "https.*mage$" | grep -vi "arm64\|aarch64" | head -1)
   ```

   The installer takes the script's variable assignments up to and including that line and runs them in the OS's built-in shell
   (src/Program/AppRunScript.hpp, `evaluate_script_variable`). That shell has pipes, `curl` (through the App Manager's network) and the text
   filters (src/Program/ShellTools.hpp): `sed`, `grep`, `tr`, `head`, `tail`, `sort` (including `-V`), `uniq`, `cut`, `wc`, `rev` and
   `awk '{print $N}'`. Their regular expressions are POSIX basic or extended, translated for the OS's regex engine. The rest of the script
   (`mkdir`, `wget`, `mv`) is never run.
3. **Download the AppImage** into `/opt/<name>/<name>`. The download goes into a `.part` file first, so a cancelled or failed one leaves
   nothing behind and an update leaves the old version in place. Anything that is not a type 1 or type 2 AppImage (a `.zip`, a `.tar.*`, a
   plain program, a web page) is refused and deleted.
4. **Write AppMan's bookkeeping:** `/opt/<name>/version` (the link, which is what Update compares) and `/opt/<name>/remove` (AppMan's
   uninstall script).
5. **Take the desktop entry and `.DirIcon` out of the image** (src/Program/AppImageLaunch.hpp, `open_appimage_image`) and write
   `/usr/local/share/applications/<name>-AM.desktop`:
   - `Exec=` becomes `/opt/<name>/<name>`, with the original arguments except `--no-sandbox` (the OS adds that itself when it is needed);
   - `Icon=` becomes `/opt/<name>/icons/<name>`, and `TryExec=` becomes the AppImage.

   An image with no desktop entry gets a plain one. Where the file system has links, `/usr/local/bin/<name>` points at the AppImage.

**Update** repeats steps 1 and 2, and steps 3 to 5 only when the link has changed. **Remove** deletes all of the above.

## The network

`desktop::appman::HttpNetwork` (src/Desktop/AppMan/HttpNetwork.hpp) is the kernel's HTTP client:
- it uses the first network interface that has an address, and the browser's trust store for https;
- it follows up to 10 redirects and sends a User-Agent (GitHub's API refuses requests without one);
- a download streams straight to the file and ends after a minute without data.

`main.cpp` gives it to the desktop with `Desktop::set_appman_network`. GitHub allows 60 API requests an hour without a token. Each install
or update check makes one.

## Threads

The App Manager window runs one job at a time on a `HandlerThread` of its own: fetching the list, installing, updating or removing. The
window stays usable meanwhile, and download progress goes to the progress bar and the status line about every 256 KiB. The worker and the
window share the catalog, the installed list and the list rows under `state_mutex_`. The UI lock is always taken before it.

## Tests

- `tests/run_host_tests.sh AppManTest` (ASan/UBSan) covers:
  - the shell's pipes and text filters: 68 command lines run by both the host's `bash` and the OS's shell, with the outputs compared;
  - the regex translation, and fuzzed patterns for every tool;
  - the catalog (and the real one when `APPMAN_CATALOG` names a copy);
  - picking the download lines out of a script;
  - the installer on a RAM file system with a stand-in network: install (the files, the desktop entry, the Start menu finding the program),
    update (up to date, a new release, a cancelled one that keeps the old version), remove, refusals (bad names, no script, no link, a link
    that is not a web address, a zip, a failed download), an image without a desktop entry, and a full disk.
- `tests/run_host_tests.sh GuiViewsTest` (`test_app_manager`) covers the window in the desktop: fetching and keeping the list, searching,
  installing, the Installed view, updating, removing, a failure message, and no network.
- `tests/appman_qemu.sh [app]` checks the whole path in the real kernel under QEMU, on the real network. It uses a build with
  `TCOS_APPMAN_SELFTEST=<app>` (its own folder, `build-appman-qemu`). Once DHCP is done the kernel fetches AppMan's catalog over https, then
  installs the application with its own HTTP client: GitHub's API, the release redirect, and the download streamed onto the primary disk. It
  reports the Start menu entry. With `appimagetool` it passed: 9,352 applications in the catalog, 14 MB installed, and "appimagetool (Linux)"
  in the Start menu.
- `tests/appman_interop.sh [-n N] [name...]` checks real install scripts from AppMan's repository. Both `bash` and the OS's shell work out
  each script's link, with `curl` answered from one cache, and the two links must match. 25 random scripts gave the same link in both.

## What is not done

- **Only AppImages.** AppMan's portable programs and archives (the `x86_64-apps` list beyond the AppImages, and downloads packed as `.zip`
  or `.tar`) are not installed.
- **Scripts that decide the link in other ways.** A script whose link depends on more than its assignments up to `version=` (a function, an
  `if`, a second variable computed after it) may not give a link; Install then says so.
- **No icons in the list,** and no "update all".
- **AppMan's `AM-updater` scripts are not written.** Update in the App Manager does their work.
- **The window itself has not been clicked through in QEMU.** The install path under it has run on the real network there (see Tests).
