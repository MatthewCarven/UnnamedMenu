# Putting `um` on a WinPE recovery drive

The plan this tool was written for: a USB stick that boots WinPE, shows a short
menu, and — depending on what you pick — partitions the disk with `diskpart`
and lays a system image back down.

This walks through the whole thing. Adjust the paths; the shape is what
matters.

> **These commands destroy data.** Everything below assumes a machine you are
> deliberately wiping. Read section 6 before you build the stick — the choice
> of what the timeout selects is the difference between a recovery drive and an
> accident waiting to happen.

---

## 1. Why `um` runs in WinPE unmodified

WinPE is a stripped Windows. Most small utilities fail there because they need
a CRT redistributable or .NET, neither of which is present.

`um.exe` imports two DLLs:

```
KERNEL32.dll
msvcrt.dll
```

Both ship in WinPE. Nothing else is needed — no VC++ redistributable, no
`WinPE-PowerShell` or `WinPE-NetFx` optional component, no install step.

Check any build you make yourself:

```bat
dumpbin /dependents dist\um.exe
```

If you build with MSVC, keep the `/MT` in `build.cmd`. It statically links the
CRT. A `/MD` build would need `vcruntime140.dll` at run time, which WinPE does
not have, and it would fail to start with nothing useful on screen.

---

## 2. Build the WinPE media

With the **Windows ADK** and the **WinPE add-on** installed, from the
*Deployment and Imaging Tools Environment* prompt:

```bat
copype amd64 C:\WinPE_amd64
```

That gives you `C:\WinPE_amd64\media\sources\boot.wim` to customise.

---

## 3. Put `um.exe` and your scripts inside boot.wim

```bat
dism /Mount-Wim /WimFile:C:\WinPE_amd64\media\sources\boot.wim ^
     /Index:1 /MountDir:C:\WinPE_amd64\mount

copy um.exe          C:\WinPE_amd64\mount\Windows\System32\
mkdir                C:\WinPE_amd64\mount\Recovery
copy scripts\*.txt   C:\WinPE_amd64\mount\Recovery\
copy scripts\*.cmd   C:\WinPE_amd64\mount\Recovery\
```

Anything inside `boot.wim` appears at `X:\` when WinPE boots, because WinPE
runs from a RAM disk mounted as `X:`. So the files above land at
`X:\Recovery\`, and `um.exe` is on the path.

Keeping the scripts *inside the WIM* rather than on the USB partition is worth
doing: `X:` is always `X:`, whereas the USB stick's own letter varies with how
many disks the machine has. See section 5.

---

## 4. Wire the menu into `startnet.cmd`

`startnet.cmd` is what WinPE runs at boot. Replace
`C:\WinPE_amd64\mount\Windows\System32\startnet.cmd` with:

```bat
@echo off
wpeinit

:menu
um -f X:\Recovery\menu.txt -d 4 -w 30
set RC=%errorlevel%

if "%RC%"=="0" goto menu
goto :eof
```

`-d 4 -w 30` highlights item 4 and selects it after thirty seconds of nobody
touching anything. Exit code 0 means cancelled, so Esc redraws the menu rather
than dropping you into a bare prompt.

And `X:\Recovery\menu.txt`:

```ini
!title  Recovery  --  choose carefully

Restore system image (WIPES DISK 0) : X:\Recovery\restore.cmd
Partition disk 0 only (WIPES DISK 0) : diskpart /s X:\Recovery\layout.txt
Capture this machine to an image     : X:\Recovery\capture.cmd
--------------------------------------------------------------
Command prompt                       : cmd.exe
Reboot                               : wpeutil reboot
Shut down                            : wpeutil shutdown
```

The dashed line is a divider: it is skipped when navigating and does not take
an index, so the safe entries stay visually separated from the destructive
ones.

Unmount when you are done editing:

```bat
dism /Unmount-Wim /MountDir:C:\WinPE_amd64\mount /Commit
MakeWinPEMedia /UFD C:\WinPE_amd64 F:
```

(`F:` being the USB stick. `MakeWinPEMedia` reformats it.)

---

## 5. The scripts

### `layout.txt` — diskpart, UEFI/GPT

```
select disk 0
clean
convert gpt
create partition efi size=260
format quick fs=fat32 label="System"
assign letter=S
create partition msr size=16
create partition primary
format quick fs=ntfs label="Windows"
assign letter=W
exit
```

For BIOS/MBR machines, `convert mbr`, a single active primary, and no EFI or
MSR partition.

### `restore.cmd` — partition, apply, make it bootable

```bat
@echo off
setlocal

rem --- find the USB partition by label rather than trusting a drive letter.
rem     WinPE itself is X:; the stick could be C:, D:, E: depending on the
rem     machine's disks, and guessing wrong here is how you apply an image
rem     from nowhere or wipe the wrong disk.
set SRC=
for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
    if exist %%D:\images\marker.txt set SRC=%%D:
)
if "%SRC%"=="" (
    echo Could not find the images volume. Is the stick still plugged in?
    pause
    exit /b 1
)

um -x -1 -d 2 -t "About to ERASE disk 0. Last chance." ^
   "Yes - wipe and restore, No - go back : rem, rem"
if errorlevel 2 exit /b 1
if not errorlevel 1 exit /b 1

diskpart /s X:\Recovery\layout.txt          || exit /b 1
dism /Apply-Image /ImageFile:%SRC%\images\baseline.wim /Index:1 /ApplyDir:W:\ || exit /b 1
W:\Windows\System32\bcdboot W:\Windows /s S: /f UEFI                          || exit /b 1

echo.
echo   Done. Remove the stick and reboot.
pause
```

Note the second `um` call: a two-entry confirmation with the *safe* answer as
the default, `-1` so a single keypress answers it. Cheap to add, and the
failure mode it prevents is expensive.

Two details in that call are easy to get wrong. `-x` is needed because run mode
returns the *command's* exit code, not the selection index. And the names use
dashes rather than commas, because a comma inside a name would split it into
two entries and the counts would stop matching.

### `capture.cmd`

```bat
@echo off
dism /Capture-Image /ImageFile:D:\images\baseline.wim /CaptureDir:C:\ ^
     /Name:"baseline" /Compress:max
pause
```

### If you would rather use `imagex`

`imagex` came from the old WAIK and DISM replaced it, but it still works if you
copy it into the image:

```bat
imagex /apply   D:\images\baseline.wim 1 W:\
imagex /capture C:\ D:\images\baseline.wim "baseline" /compress maximum
```

`dism /Apply-Image` is the supported path on anything current and handles
WIMBoot and split WIMs that `imagex` does not.

---

## 6. Which entry should the timeout pick?

Make the timeout default something **harmless**. A recovery drive left plugged
in, in a machine that reboots on its own, will sit at that menu with nobody
watching — and whatever `-d` points at is what happens.

In the menu above, `-d 4` is `Command prompt`. Someone walking up to a finished
machine finds a prompt. The alternative — defaulting to the restore entry —
means an unattended reboot silently reformats a disk.

The other habits worth keeping:

* Put destructive entries **above** the divider and name them so the
  consequence is in the label: `Restore system image (WIPES DISK 0)`, not
  `Restore`.
* Confirm destructive actions with a second `um` call whose default is "no".
* Do not use `-1` (instant hotkeys) on the main menu. A single mistyped key
  should not be able to start a wipe; Enter is worth the extra keystroke there.
  Use `-1` on the yes/no confirmation, where the default is safe anyway.

---

## 7. Testing without burning a stick

Boot the ISO in a VM with no disks attached, or attach a scratch VHD:

```bat
MakeWinPEMedia /ISO C:\WinPE_amd64 C:\WinPE_amd64\winpe.iso
```

You can also check the menu itself on your normal desktop first — `um` behaves
identically there, and `um -f menu.txt --dump` shows exactly what it parsed
without running anything.
