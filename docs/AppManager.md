# App Manager

The App Manager is the desktop's package manager. Open it from the Start menu ("App Manager"). It lists the applications of two catalogs in one
list, and the **Type** column says which kind a row is:

- **Linux**: the AppImages of [AppMan](https://github.com/ivan-hc/AM) (the `am` / `appman` tool), about 9,000, with an install script for each.
- **Windows**: the packages of [winget](https://github.com/microsoft/winget-pkgs), the Windows Package Manager, about 16,000.

What the window does:

- **Search** narrows the list by name and description as you type (for a Windows package: its name, its identifier such as `Mozilla.Firefox`, and
  its short name). Names that start with the search come first, whichever kind they are.
- **Show** switches between all applications, the installed ones, the ones for Linux and the ones for Windows. The View menu has the same
  choices (Installed Only, For Linux Only, For Windows Only).
- **Install**, **Update** and **Remove** act on the chosen application. Installing also happens on a double click. **Cancel** stops a
  download.
- **Refresh list** fetches both catalogs again. Copies are kept in `/AppStore/AppMan` (`stats-appimages` and `winget-packages`), so the list is
  there at once next time. If one catalog cannot be fetched, the other is still used and the status line says so.
- The **Status** column says "Installed", or "Update available" for a Windows package whose catalog version is newer than the installed one.

An installed Linux program appears in the Start menu as "Name (Linux)" and runs like any AppImage (see AppImage.md): mounted privately, with its
AppRun worked out, and Electron programs get `--no-sandbox`. An installed Windows program appears as "Name (Windows)" and runs as any Windows
program does (see WindowsUserMode.md).

## How a Linux program is installed

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

## How a Windows program is installed

`desktop::appman::WingetInstaller` (src/Desktop/AppMan/WingetInstaller.hpp) works from winget's published source,
`https://cdn.winget.microsoft.com/cache`, the one the real `winget` reads.

**The catalog** is `source2.msix`: a zip archive with ZIP64 records (src/Media/Zip.hpp) that holds `Public/index.db`, an SQLite database
(src/Desktop/AppMan/SqliteReader.hpp, a read-only reader of rowid tables). Its `packages` table has one row per package: identifier, name,
moniker, latest version, and the SHA-256 of the package's version list. `WingetCatalog` reads it and keeps it as a text file, one line per
package, so the next start does not unpack a database.

**An install** then takes five steps:

1. **Fetch the version list**, `packages/<identifier>/<first 8 hex digits of the hash>/versionData.mszyml`. It must have the SHA-256 the index
   names. It is YAML packed with MSZIP (blocks of 32 KiB, each deflated with the ones before it as its dictionary) and gives, for each
   version, the path and SHA-256 of its manifest.
2. **Fetch the manifest** of the latest version. It must have the SHA-256 the version list names. It is YAML
   (src/Desktop/AppMan/Yaml.hpp, the part of YAML that winget's files use) and says what to download for which CPU, the SHA-256 of that, and
   what kind of installer it is.
3. **Choose the installer**: x64, else neutral (the Windows layer runs 64-bit programs only, so x86 installers are not taken). One of the kinds
   the App Manager puts in place itself is taken first:
   - `portable`: one `.exe`;
   - `zip` with `NestedInstallerType: portable`: an archive of portable programs.

   Otherwise a setup program, see below.
4. **Download it** into the package's folder, through `download.part`, and check its SHA-256 against the manifest. Every byte that is installed
   is therefore the one the catalog names, from the index down.
5. **Put it in place**, in `C:\Program Files\WinGet\Packages\<identifier>_Microsoft.Winget.Source_8wekyb3d8bbwe`, which is where winget puts
   portable packages installed for all users:
   - a portable program is named after its command (`jq.exe`), else after the downloaded file;
   - an archive is unpacked whole. A member whose name would leave the folder (`..`, an absolute path, a drive) makes the whole archive be
     refused;
   - each program gets a shortcut in `C:\ProgramData\Microsoft\Windows\Start Menu\Programs` (`write_shell_link` in
     src/Desktop/InstalledApps.hpp), which is what the desktop's Start menu lists. One program is called after the package; several are called
     after their command aliases. A shortcut of the same name that belongs to something else is left alone;
   - `winget-install.txt` in the folder records the identifier, name, version and the shortcuts.

**Update** does nothing when the installed version is the catalog's (no network needed). Otherwise it repeats steps 1 to 5; the old files go
only after the new download has been checked. **Remove** deletes the shortcuts the record names and the folder.

### Packages that come as a setup program

A package of the kind `inno` (Inno Setup), `nullsoft` (NSIS) or `exe` is installed by running its setup program in the Windows layer. The App
Manager reaches that through `appman::ISetupHost` (src/Desktop/AppMan/SetupHost.hpp); the kernel's is `KernelSetupHost`, which runs the program
with `run_program` and reads the registry's Uninstall keys (`list_uninstall_entries` in src/Process/WindowsRegistry.hpp).

1. Steps 1 and 2 above, then the setup program is **downloaded** into `C:\Windows\Temp\WinGet\<identifier>.<version>` and its SHA-256 checked.
   It must be a 64-bit Windows program.
2. It is **run with the switches that make it ask nothing**: the manifest's `InstallerSwitches` (`Silent`, else `SilentWithProgress`), else the
   usual ones of its kind (`/SP- /VERYSILENT /SUPPRESSMSGBOXES /NORESTART` for Inno Setup, `/S` for NSIS), then the manifest's `Custom` switches.
   The status line says "Running the setup program", and Cancel stops it. The setup program (and an uninstaller, and what they start) is run
   An Inno Setup program is also asked to keep a log (`/LOG=`, in the folder of the downloaded setup program); when it fails, what the message
   says is its exit code and what the log says from the first error it reports (six lines at most, plain text only), else the log's last three
   lines. It is run **as an administrator**: with the elevated form of the user's token, where every other program has the limited one. The user asked for the
   installation; a setup program that finds itself not elevated asks Windows to start it again elevated, which nothing here would answer.
3. It **succeeded** if it ended with 0 or one of the manifest's `InstallerSuccessCodes`.
4. The registry's **Uninstall entries** are read before and after. The entry the setup program added is the package's (with several, the one
   with the manifest's `ProductCode`, else the one named like the package); for a program that was already installed, the entry the manifest
   names.
5. The **record** is `/AppStore/AppMan/Winget/<identifier>.txt`: identifier, name, version, kind, and the uninstall command of that entry (its
   `QuietUninstallString` if it has one). It is a file of the App Manager's because this system's registry is kept in memory and does not
   outlast a restart. The downloaded setup program is deleted.

**Update** runs the new version's setup program, which replaces the old version itself. A package stays of the kind it was installed as.
**Remove** runs the recorded uninstall command, with the quiet switches of its kind unless it is a quiet command already, and then forgets the
package. If the uninstaller is no longer on the disk the package is just forgotten. If the setup program registered no uninstaller, Remove says so;
choosing Remove a second time forgets the package.

### Packages that are Windows Installer packages (.msi)

A package of the kind `msi` or `wix` is not a program but a description, and the App Manager installs it itself with this system's own
Windows Installer engine (see WindowsInstaller.md), as `msiexec /i ... /qn` would:

1. Steps 1 and 2 above, then the `.msi` is **downloaded** into `C:\Windows\Temp\WinGet\<identifier>.<version>` and its SHA-256 checked.
2. Its **execute sequence is run**: files out of the cabinets inside it, registry values, shortcuts, the entry of installed programs, and the
   programs the package itself runs (the status line says "Running the setup program" meanwhile, and Cancel stops it). The manifest's switches
   that are `NAME=value` become properties of the installation; the switches of `msiexec` itself (`/qn`, `/norestart`) say nothing this does
   not do anyway.
3. The package is **kept** as `C:\Windows\Installer\<product code>.msi`, because removing the product is running the same package the other
   way. The **record** (`/AppStore/AppMan/Winget/<identifier>.txt`) has `InstallerType: msi`, the product code, the kept package, and a
   `Note:` line for everything the engine skipped (custom actions that are DLLs, services, environment variables, ...).

If an action fails, what was put in place is taken away again and the message says which step failed. **Update** installs the new version;
when that is another product (most are), the old one is removed first as "being upgraded". **Remove** runs the kept package as a removal and
then forgets it. Where a setup program of a kind that is known to run here is offered as well (`inno`, `nullsoft`), that one is taken.

Refused, with the reason in the message:

- `burn`, `msix`, `appx` and Store packages; a Windows Installer package whose files are not inside it, or that is for another processor;
- a setup program that is a 32-bit program of no known kind (`exe`). Many setup programs are 32-bit, even for 64-bit applications - of five
  popular ones looked at, 7-Zip's and Git's are 64-bit, Firefox's, Visual Studio Code's and HandBrake's are 32-bit - and the 32-bit ones of
  Nullsoft (`nullsoft`) and Inno Setup (`inno`) are run (see Wow64.md for what 32-bit programs have here);
- an `exe` installer whose manifest has no silent switch;
- packages with no x64 or neutral installer.

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

Windows side:

- `tests/run_host_tests.sh AppManTest` (ASan/UBSan) also covers, with the stand-in winget source of `tests/data/winget` (made by
  `make_fixtures.py` there):
  - the zip reader (ZIP64 and plain archives, stored and deflated members, the CRC), the SQLite reader (a table of several levels, a row with
    overflow pages), the YAML parser, the MSZIP decoder (a version list of three blocks) and the kept list. Each is also run on its input cut
    short at many lengths and with bits flipped;
  - the catalog: identifiers, search order, the kept list;
  - manifests, the choice of installer, archive member names, and a shortcut written and read back;
  - setup programs, with a stand-in for the Windows layer: the switches of each kind and of the manifest, success codes, which registry entry
    is the package's, the record, update, remove, a 32-bit Nullsoft setup program and its uninstaller, and the refusals and failures (a 32-bit Inno Setup program, a download that is no package, no silent switch, a wrong checksum, an exit code,
    a program that does not start, cancelling, a missing or failing uninstaller, no uninstaller, an edited record);
  - a Windows Installer package (two versions that are two products): install, the programs it runs, the Start menu, the record and the kept
    package, update, remove, a failing step and a cancelled one that leave nothing, an edited record;
  - the installer on a RAM file system: a portable program, update (cancelled, done, up to date), an archive of two programs, an archive of
    one, an identifier that is escaped in the address, remove, and the refusals (a setup program, ARM only, a wrong checksum, an archive with
    `../`, a download that is not a program, a forged version list and manifest, a missing download, a cancelled install, a full disk). None of
    the refusals leaves a file behind.
- `tests/run_host_tests.sh GuiViewsTest` (`test_app_manager`) covers both kinds in the window: the Type column, the merged search, the Show
  filter, installing, updating and removing a Windows program, and the Installed view with both kinds.
- `tests/winget_interop.sh [id...] [--install id...]` checks against the real source on the network. It read the real index (16,396 packages),
  fetched and verified the manifests of `Mozilla.Firefox`, `Notepad++.Notepad++`, `7zip.7zip` and `Allen&Heath.SQMixPad`, and installed `jq`,
  Notepad++ and Sysinternals Autoruns into a RAM file system, where the Start menu found them. (Sysinternals Process Explorer was refused: the
  vendor's file no longer has the SHA-256 its manifest names.)

- `tests/winget_qemu.sh [package]` checks the Windows side in the real kernel under QEMU, on the real network. It uses a build with
  `TCOS_WINGET_SELFTEST=<package>` (its own folder, `build-winget-qemu`): once DHCP is done the kernel fetches and reads winget's index, installs
  the package, reports what the Start menu lists, and removes it again. It passed with:
  - `jqlang.jq` (a portable program): 16,396 packages in the index, 1 MB installed, "jq (Windows)" in the Start menu, removed;
  - `7zip.7zip` (a 64-bit setup program): downloaded, run with `/S`, "7-Zip File Manager (Windows)" in the Start menu, then removed by its own
    uninstaller.

  - `KeePassXCTeam.KeePassXC` (a Windows Installer package, 69 MB, an LZX cabinet): installed by this system's own engine, "KeePassXC (Windows)"
    in the Start menu, removed again with the kept package. Its first run found that the kernel's registry makes only the last key of a path;
    the installer's registry functions now make a key's parents one by one.
  - `Google.Chrome` (a Windows Installer package that wraps Google's updater): **installed and removed.** The engine unpacks the updater's
    setup program and runs it with the command line the package builds; that program sets the updater up, starts its service and asks it,
    through COM, to run the Chrome installer the package carries; the installer unpacks Chrome (about 400 MB) into
    `C:\Program Files\Google\Chrome\Application` and writes its shortcuts. The Start menu then lists "Google Chrome (Windows)". Removing runs
    the package the other way, which runs Chrome's own uninstaller. The run takes about four minutes in QEMU. What it took, beyond the
    package engine: five faults of the 32-bit layer (docs/Wow64.md), COM between processes (docs/WindowsCom.md), MSXML (the updater reads
    its manifest with it), `cabinet.dll` (Chrome's installer unpacks its setup program with it), file mappings larger than 64 MiB (the
    installer maps its own 160 MiB file), the "downlevel" API set names, a property store on shortcuts, and details of services and
    registry security. Not done: the updater's scheduled tasks (there is no Task Scheduler; it goes on without), its firewall rule, and
    whether the installed Chrome then *runs* is the subject of docs on the Chromium work, not of this. The run was made with the checksum
    check off, because Google had already replaced the file that winget's manifest names: until the manifest follows, the App Manager
    refuses the download, as winget itself does.

  The 7-Zip run found two faults that are fixed: this system's `msvcrt.dll` did not export the data `_fmode`, `_commode` and `_acmdln` that the
  startup code of older programs imports (the setup program crashed at once), and a program that an ended program had started went on using what
  its parent's run owned (an uninstaller hands its work to a copy of itself and ends; the kernel then stopped with a page fault). A setup run now
  waits for every program it started (`ProgramRun::wait_for_descendants`).

Linux side:

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

Windows:

- **Windows Installer packages are installed as far as their tables say** (WindowsInstaller.md): no DLL or script custom actions, services,
  environment variables, COM registration or file types. A package that is a wrapper around a setup program runs that program, and whether
  it works is up to the Windows layer.
- **Setup programs only where the Windows layer can run them.** Of the 32-bit ones Nullsoft's and Inno Setup's; no MSIX or Burn (see above). What a setup
  program wrote to the registry is gone after a restart (the registry is kept in memory), so a program that needs its registry settings may not
  start then; the App Manager's own record, and with it Remove, is kept.
- **A setup program's shortcuts and files are its own business.** The App Manager does not check what it installed, and a setup program that
  shows a window although asked not to waits for the user.
- **A download that changed is refused.** Some vendors (Google for Chrome, for one) replace the file behind a fixed address with every release,
  and until winget's manifest follows, the SHA-256 does not match and the package cannot be installed - by the real winget either.
- **A few packages have been through QEMU** (see Tests): a Windows Installer package (KeePassXC), a portable program (jq), a 64-bit setup program (7-Zip), 32-bit Nullsoft ones
  (qBittorrent) and 32-bit Inno Setup ones (WinMerge, TreeSize Free). That is
  four setup programs out of thousands; the App Manager's window has not been clicked through there, and no installed program has been
  started from the Start menu there.
- **An archive member is unpacked in memory**, so one file of an archive can be at most 1 GiB and needs that much memory.
- **No dependencies** (a package's `Dependencies`, such as the Visual C++ runtime, are not installed), no command aliases in a `Links` folder,
  no descriptions in the list (winget's index has none), and only the latest version of a package can be installed.
- **A failed update after the download has been checked** (a full disk while unpacking) removes the package instead of leaving the old version.

Linux:

- **Only AppImages.** AppMan's portable programs and archives (the `x86_64-apps` list beyond the AppImages, and downloads packed as `.zip`
  or `.tar`) are not installed.
- **Scripts that decide the link in other ways.** A script whose link depends on more than its assignments up to `version=` (a function, an
  `if`, a second variable computed after it) may not give a link; Install then says so.
- **No icons in the list,** and no "update all".
- **AppMan's `AM-updater` scripts are not written.** Update in the App Manager does their work.
- **The window itself has not been clicked through in QEMU.** The install path under it has run on the real network there (see Tests).
