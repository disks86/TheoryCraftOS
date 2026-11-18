# Windows Installer packages (.msi)

A Windows Installer package is not a program. It is a description: a small database of tables (folders, features, components, files, registry
values, shortcuts) and a list of actions to run in order. On Windows the service behind `msiexec` reads it and does the work. Here the App
Manager does, with an engine of this system's own in `src/Msi`, so a package installs with nothing of Windows Installer present.

This is what installs the packages winget lists as `msi` or `wix` (see AppManager.md). It is the equivalent of `msiexec /i package.msi /qn` and
`msiexec /x package.msi /qn`: no questions and no windows.

## The parts

| Part | Where | What it does |
| --- | --- | --- |
| Compound file reader | `src/Media/CompoundFile.hpp` | A `.msi` is a compound file ([MS-CFB], "OLE2 structured storage"): sectors chained by an allocation table, a directory of named streams, small streams in a mini stream. Versions 3 and 4. A stream is read in place at any offset, so a cabinet of hundreds of megabytes is never loaded. |
| Cabinet reader | `src/Media/Cabinet.hpp`, `src/Media/Lzx.hpp` | The files of a package are in cabinets (`MSCF`) inside it: stored, MSZIP or LZX folders. Files are handed to a sink as the folder is decoded, with only one block and the decoder's history in memory. Identical files that the cabinet keeps once under several names are copied. |
| Database | `src/Msi/MsiDatabase.hpp` | Stream names are packed; every text is in one string pool; `_Tables` and `_Columns` describe the tables; rows are stored column by column. Read into memory and answered by table, row and column name, as UTF-8. Also the summary information (the processor the package is for). |
| Conditions and formatted text | `src/Msi/MsiExpression.hpp` | `VersionNT >= 601 AND NOT Installed`, `[INSTALLDIR]bin\[#MainExe]`. |
| Engine | `src/Msi/MsiInstaller.hpp` | Runs the `InstallExecuteSequence`. |

The engine reaches the Windows world through `msi::IHost`: the registry, running a program and waiting for it, and writing a shortcut. In the
kernel that is `KernelSetupHost` (the registry functions at the end of `src/Process/WindowsRegistry.hpp`, the program runner); in the tests
it is a stand-in.

## What the engine does

- **Properties.** The package's `Property` table, then what Windows Installer defines (the folders of the system such as `ProgramFiles64Folder`
  and `ProgramMenuFolder`, `VersionNT=603` and `VersionNT64` as Windows 10 reports them, `Privileged`, `UILevel=2`), then `NAME=value` pairs
  from the caller (the App Manager passes the ones in the manifest's switches). `ALLUSERS` is always 1: every installation is for all users.
- **AppSearch** for registry values (`RegLocator`), and **LaunchConditions**: a condition that is false stops the installation with the
  package's own message.
- **Costing.** Every row of `Directory` becomes a path on drive C (`/Windows` here). A feature is chosen by its level against `INSTALLLEVEL`,
  by the rows of the `Condition` table, or by `ADDLOCAL`; a component is installed if a chosen feature has it and its condition holds.
- **InstallFiles** from the cabinets inside the package (`Media` says which cabinet holds which sequence numbers), **CreateFolders**,
  **WriteRegistryValues** (text, `#` numbers, `#x` bytes, `#%` expandable text, `[~]` lists, keys made with `+` and `*`), **RemoveRegistry**,
  **CreateShortcuts** (to a formatted path, or to the key file of a component when the target is a feature), **RemoveFile**, and
  **RegisterProduct**: the entry under the registry's `Uninstall` key, named by the product code.
- **Custom actions** of the kinds that need nothing but the tables: set a property (type 51), place a folder (35), stop with a message (19),
  and run a program: one kept in the package's `Binary` table (type 2, unpacked into `C:\Windows\Installer`, run, deleted), an installed file
  (18), a path in a property (50) or a command line (34). The program's exit code decides whether the installation goes on, unless the
  action says its result does not matter.
- **Removal** is the same sequence run with `REMOVE=ALL` and `Installed` set: `RemoveFiles`, `RemoveFolders` (empty ones the package named,
  never a folder of the system), `RemoveRegistryValues`, `RemoveShortcuts`, the custom actions the package runs when it goes, and the
  `Uninstall` entry. That is why the App Manager keeps the package, as Windows does, in `C:\Windows\Installer\<product code>.msi`.

## What it does not do

It says so rather than guess: each of these is a line in the App Manager's record of the package (`Note: ...`).

- **Custom actions that are DLLs or scripts are skipped.** They call back into Windows Installer (`MsiGetProperty`, `MsiSetProperty`, ...) from
  inside its process, and there is no `msi.dll` behind them here. Most are housekeeping (closing a running copy, broadcasting a change) and
  the product works without them; a package whose real work is in one will install files and then not work.
- **No services, environment variables, COM classes, file types, fonts, `.ini` values, ODBC or assemblies** (`ServiceInstall`,
  `Environment`, `Class`, `Extension`, `ProgId`, `TypeLib`, `SelfReg`, `Font`, `IniFile`, ...). A program that needs to be on `PATH` is not.
- **No rollback.** If an action fails, what was put in place is taken away again by running the package as a removal that does not run the
  package's programs. Files that the installation replaced are not brought back.
- **No patches, transforms, merge modules, nested or advertised installations**; no `msiexec` program and no `msi.dll` for Windows programs
  that want to install a package themselves (a WiX Burn bundle, for one, is still refused).
- **Files must be inside the package.** A package whose cabinets or files lie beside it is refused, as is one for another processor.
- **Versions of files are not compared**: a file is written whatever is there. Components are not counted across products.
- **Text is code page 1252 or UTF-8.** Of a package in another code page only the ASCII characters are kept (names of files included).
- The registry does not outlast a restart (see AppManager.md), so what a package wrote there is gone then; the files and shortcuts stay.

## Untrusted input

A package is a download. Sector chains cannot loop and tables are size-limited; every cell, string number and column is checked; a folder
or file name that would leave drive C, go through `..` or hold a character Windows does not allow stops the installation; a file must have
the size the package's table gives; the kept package named in the App Manager's record must be a file in `C:\Windows\Installer`.

## Tests

- `tests/run_host_tests.sh MsiTest` (ASan/UBSan). The packages of `tests/data/msi` are written from nothing by `make_fixtures.py` there
  (compound file, tables, cabinet), which also has 7z list them and cabextract unpack their cabinets, so another reader agrees about the
  format. The test reads them, checks conditions and formatted text, installs into a RAM file system and checks every file, registry value,
  shortcut and program run, removes again, goes through the refusals and failures, and runs about 2,600 packages cut short or with bits
  flipped through the reader and the engine.
  - `MsiTest --list <file.msi>` prints a real package's properties, custom actions and sequence; `MsiTest --install <file.msi>` installs one
    into a RAM file system and removes it again, saying what was written, run and skipped.
- `tests/run_host_tests.sh CabinetTest`: the cabinet reader and the LZX decoder (fixtures checked against cabextract; `CABINET_TEST_FILES`
  compares real cabinets with what cabextract makes of them).
- `tests/run_host_tests.sh AppManTest`: the way from winget's catalog to the installed product, the kept package, an update to a version
  that is another product, removal, a failure that leaves nothing, an edited record.
- `tests/winget_interop.sh --install <id>` installs real packages from the network into a RAM file system (the programs a package runs are
  only listed), and `tests/winget_qemu.sh <id>` does it in the real kernel.

Real packages tried (November 2026): Node.js LTS (1,938 files, of which the cabinet holds 202 once and lists the rest as the same bytes),
KeePassXC and Eclipse Temurin install and remove on the host; KeePassXC also in the real kernel under QEMU. Google Chrome's package is read and
run correctly, and fails in the program it wraps (see AppManager.md).
