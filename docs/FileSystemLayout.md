# Filesystem layout

The system filesystem (`SystemFileSystem`, the tree the file explorer and every program see) has a fixed layout. The paths are
named in [`src/FileSystem/SystemLayout.hpp`](../src/FileSystem/SystemLayout.hpp): use those names instead of spelling a path out.

| Path | What it is |
| --- | --- |
| `/` | The root of the filesystem: the primary partition (the first writable, built-in, non-boot volume). |
| `/EFI` | A mount of the EFI system partition. It is always called `EFI`, whatever its label says. |
| `/Desktop` | Everything that is on the user's desktop. |
| `/Desktop/Downloads` | Default location for what the browser downloads from the internet. |
| `/Desktop/Documents` | Default location for spreadsheets, text files and similar documents. |
| `/Desktop/Pictures` | Default location for image files. |
| `/Desktop/Video` | Default location for video files. |
| `/Desktop/Audio` | Default location for audio files such as music. |
| `/Windows` | Root for Windows user-mode processes: they see its contents as `C:\`. |
| `/Linux` | Root for Linux processes: they see its contents as `/`. |
| `/AppStore` | Where applications keep the files they need. Each application has its own sub-folder, e.g. the browser's cache is `/AppStore/Browser/Cache`. |

Other volumes (USB sticks, network shares) are folders of `/` named by their label.

## Rules for new code

- A default save location comes from the table above: browser downloads go to `/Desktop/Downloads`, documents to
  `/Desktop/Documents`, and so on. Use the constants in `filesystem::layout`, not string literals.
- An application stores its own files (caches, settings, databases, state) under `/AppStore/<Application>/`, never directly in `/` or in
  another application's folder. `layout::app_store_path()` builds the path and `layout::create_app_folder()` makes the folder.
- Do not create new top-level folders in `/`. If something does not fit the table, extend the table (this file and `SystemLayout.hpp`)
  in the same change.
- Windows and Linux processes get a view of their own root (`/Windows` as `C:\`, `/Linux` as `/`). Translate paths at the system-call
  boundary; a process must not be able to name a path outside its root.
- `/EFI` is a mount, not a folder of the primary partition. Do not create an `EFI` folder on the primary partition (it would be hidden by
  the mount).

## How it is built

`SystemFileSystem` mounts every volume on the machine. A volume that has an `/EFI` folder is the boot partition and is mounted as
`EFI`; the first writable built-in volume that is not a boot partition becomes the root volume, and the folders of the table (except
`/EFI`) are created on it when they are missing (`layout::create_folders()`). Existing folders and their contents are left alone.

With no primary partition (a machine with only the EFI partition, or no usable storage) the root is read-only and lists the volumes. On a
machine with no storage at all the in-memory fallback volume `Memory` holds the same folders itself (`/Memory/Desktop/...`); its contents
are lost at power-off.
