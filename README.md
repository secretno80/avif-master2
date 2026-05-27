# AVIF-Master — Technical Specification & Developer Guide

**Language / 언어:** English | [한국어](README.ko.md)

AVIF-Master is a low-level hybrid image conversion tool for Windows.  
It combines a lightweight Win32 GUI, in-memory archive stream processing, GPU VRAM caching, and an OpenCL preprocessing pipeline to convert large batches of images and archived files at maximum speed with minimal latency.

**External tools used:**
- `avifenc` / `avifdec` for AVIF encoding and decoding
- Windows built-in tools (`tar.exe`, PowerShell) for archive extraction

---

## Part 1: System Overview & Architecture

### 1. Project Overview

AVIF-Master targets the following goals:

| Goal | Approach |
|------|----------|
| Lightweight GUI | Win32 API (`WC_LISTVIEW`, `WM_DROPFILES`, etc.) — no framework dependency |
| Fast archive handling | Windows built-in `tar.exe` and PowerShell `ZipFile` API |
| High-throughput conversion | Multi-process `avifenc` spawning with atomic progress tracking |
| Explorer integration | Right-click context menu via HKCR registry (`*\shell`, `Directory\shell`) |

### 2. Master-Slave Instance Management (Data-Loss Prevention)

When multiple files are selected and launched from Explorer's context menu, Windows may spawn multiple processes sequentially. The following synchronization architecture prevents data loss.

#### 2.1 Adaptive Settling Time

- **Master election**: The first process to acquire the Global Mutex (`CreateMutexW`) becomes the Master and maintains the GUI message loop.
- **Slave behavior**: Subsequent (Slave) processes write their file path arguments to a uniquely named `.args` file (PID-based naming) in `%TEMP%` and exit immediately.
- **Adaptive wait**: The Master polls for new `.args` files after startup. Once no new file appears for 300–500 ms after the last arrival, it collects all `.args` files atomically, registers them in the grid, and starts worker threads.

#### 2.2 IPC (Inter-Process Communication)

After the initial settling phase, any subsequently launched instance forwards its file list to the Master window via `WM_COPYDATA`.

#### 2.3 Auto-Convert Mode

Pass `/auto` or `--auto-convert` on the command line to start conversion automatically after file collection completes.

- File collection starts → Settling Time wait → Collection complete → **Conversion starts automatically**
- Context menu entry "Convert with AVIF-Master (Start Now)" activates auto mode.
- The plain "Convert with AVIF-Master" entry adds files only; the user confirms before starting.

---

## Part 2: Feature & UI Specification

### 3. Main Grid (ListView) Columns

Uses Virtual ListView (`LVS_OWNERDATA`) to minimize resource usage. Each column is updated atomically by worker threads.

| Column | Description |
|--------|-------------|
| Name | File or archive name (system icon lazy-loaded via `SHGetFileInfo`) |
| Type | Format (JPG, PNG, WebP, ZIP, 7Z, etc.) detected by extension |
| Original Size | Pre-conversion size in KB/MB/GB (`GetFileAttributesExW`, no file open) |
| Status | Real-time state: Pending / Collecting... / Converting (%) / Done / Failed |
| Result Size | Actual AVIF output file size after successful conversion |
| Reduction | Savings ratio (%): `(original − result) / original × 100` |

### 4. Archive Processing Strategy

Archives (ZIP/7Z) are registered as a **single row**. Internal progress is shown inline (e.g., `[Converting] 45/200`).

After conversion, the archive is repacked in the same format (`.zip` or `.7z`) with only the converted AVIF files replaced inside.

#### 4.1 Archive Processing Flow

1. **Extract** to a temp folder (status: "Extracting...")
2. **Convert** each image in-place (status: "Converting X/N")
3. **Backup** original to `_backup` name if "Preserve archive backup" is enabled
4. **Repack** into the same format (status: "Repacking...")
5. **Log** results if "Save conversion log" is enabled:
   - Path: `<source_dir>\AVIF_Conversion_Logs\YYYYMMDD_HHMMSS_convert.log`
   - Contents: file names, timestamps, success/failure counts, list of failed files

---

## Part 3: UI Layout & Option Details

### 5. Main Window Layout

The main window is divided into three functional areas.

#### 5.1 Grid Area (Top)

- Virtual ListView (`LVS_OWNERDATA`) with per-row checkboxes for batch selection.
- Drag & Drop via `WM_DROPFILES`.

#### 5.2 Settings Panel (Right)

Organized into **Performance** and **File Management** tabs.

**Performance Tab**

| Option | Description |
|--------|-------------|
| Hardware mode | **CPU Only** (quality-first) / **CPU + GPU Hybrid** (OpenCL preprocessing) |
| GPU Caching | Enables asynchronous VRAM prefetching (GPU mode only) |
| CPU Batching | Configure Concurrent Jobs and Threads per Job for the hardware |
| SIMD/Assembly | Force-enable or disable AV1 encoder CPU instruction optimizations |
| Quality Preset | ⚡ Fast (`--preset 8`) / ⚙️ Normal (`--preset 6`) / 🎯 High (`--preset 4`) |

**File Management Tab**

| Option | Description |
|--------|-------------|
| Output path | Same folder / Specific folder / Preserve subfolder structure |
| Keep modified date | Sets the output file's modified date to match the source file |
| Skip duplicates | Skip if an `.avif` with the same name already exists |
| Denoise filter | GPU OpenCL `fastNlMeansDenoising` texture noise removal |
| Resize | LANCZOS3 filter with configurable target resolution |
| Auto-close | Automatically exits the program after conversion completes |
| Archive backup | Preserve original archive as `_backup` before repacking (ZIP/7Z) |
| Conversion log | Save timestamped log; auto-deleted on successful completion |
| Profile save/load | Save or load the current settings profile |

#### 5.3 Status Bar (Bottom)

Full-width smooth progress bar.

- Left: Overall state (Idle / In Progress... / Done / Failed)
- Right: Current throughput (TPS/FPS) and estimated time remaining (ETA)

### 6. Completion Report Dashboard

A summary dialog shown after all conversions finish:

- **Total elapsed time** (millisecond precision)
- **Success / failure counts** with links to detailed error logs
- **Size change**: total original → total result size with savings % and a bar chart

---

## Part 4: Build, Installation & Deployment

### 7. Context Menu Integration

Right-click menu label: **"Convert with AVIF-Master"**

Registered via HKCR registry keys (`*\shell`, `Directory\shell`, `Folder\shell`, `Directory\Background\shell`). The `%1` argument is passed to the Master process, which uses Adaptive Settling Time (300–500 ms) to collect all targets before registering them in the grid.

### 8. Build & Packaging

```bat
build.bat
```

Compiles `src/main.cpp` with MinGW (`g++ -mwindows -municode`) and links required Windows system libraries.  
The resulting `build/AVIFMaster2.exe` is then packaged into a Windows installer via the Inno Setup script (`setup.iss`).  
Uninstallation fully removes all files and registry entries with no leftovers.

### 9. Keep Modified Date — Behavior & Verification

#### Behavior

- When **enabled**: after a successful conversion the output file's modified date is set to match the source file's modified date.
- When **disabled**: the output file retains the standard creation timestamp (time of conversion).
- If reading or writing the date fails, a failure note is appended to the item's status string.
- On successful conversion, the item's `.log` file is automatically deleted.

#### Manual Verification (GUI)

1. Prepare a source image and note its modified date in file properties.
2. Launch the program, add the file, and enable **Keep modified date**.
3. Run conversion and verify the output file's modified date matches the source.
4. Disable the option, convert again, and confirm the output date reflects the current time.

#### Manual Verification (Auto-Convert Mode)

1. Set `KeepModifiedTime` = `1` under `HKCU\Software\AVIFMaster2` in the registry.
2. Run `AVIFMaster2.exe /auto <source_path>` and verify the output modified date.
3. Set `KeepModifiedTime` = `0` and repeat to confirm the ON/OFF difference.

---

## Third-Party Licenses

This project includes or depends on the following open-source libraries.  
Full license texts are available in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

| Library | Version  | License      | Usage                                    |
|---------|----------|--------------|------------------------------------------|
| libavif | 1.4.0    | BSD 2-Clause | AVIF encode/decode binaries (`lib/`)     |
| libaom  | 3.13.1   | BSD 2-Clause | AV1 encoder (bundled in avifenc)         |
| dav1d   | 1.5.3    | BSD 2-Clause | AV1 decoder (bundled in avifenc/avifdec) |
| libyuv  | rev.1922 | BSD 3-Clause | YUV color space processing (in libavif)  |
| libwebp | (source) | BSD 3-Clause | WebP support (`Dependencies/libwebp/`)   |

## Credits

- Project owner / distributor: secretno80
- Development assistance: GitHub Copilot, Claude

## License

This project is licensed under the [MIT License](LICENSE).
