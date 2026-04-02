# QEMU Snapshot Web Manager — User Guide

Welcome! This guide will walk you through everything you need to know to manage
your virtual machine snapshots with a friendly web interface. No command-line
wizardry required (well, maybe just a tiny bit to get started).

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Getting Started](#2-getting-started)
3. [The Interface](#3-the-interface)
4. [Managing Your VMs](#4-managing-your-vms)
5. [Working with Snapshots](#5-working-with-snapshots)
6. [Shared Folders](#6-shared-folders)
7. [Tips & Best Practices](#7-tips--best-practices)
8. [Troubleshooting](#8-troubleshooting)
9. [Glossary](#9-glossary)

---

## 1. Introduction

### What is QEMU Snapshot Web Manager?

QEMU Snapshot Web Manager (QSWM) is a lightweight web tool that lets you create,
view, and manage snapshots for your QEMU/KVM virtual machines — all from your
web browser. Think of it as a control panel for your VMs' save points.

### What are snapshots?

A **snapshot** is like a save point in a video game. It captures the exact state
of your virtual machine at a moment in time — what's on the disk, what programs
are running, everything. If something goes wrong later, you can jump right back
to that save point as if nothing happened.

If you've used VirtualBox before, it's the same idea — but for QEMU/KVM virtual
machines.

### Why this tool?

QEMU/KVM is powerful, but it doesn't come with a nice visual way to manage
snapshots. You'd normally have to type commands in a terminal to create, list,
or restore them. This tool gives you a clean web interface where you can see all
your snapshots laid out as a tree, click buttons to create or restore them, and
manage your VMs — no terminal commands needed after setup.

---

## 2. Getting Started

### What you need

- A **Linux computer** running QEMU/KVM virtual machines.
  Tested on Fedora, Ubuntu, and Debian, but it should work on most Linux
  distributions.
- The **libvirt** service running (this is the software that talks to your VMs
  behind the scenes — if you're already running VMs with `virt-manager`, you
  have it).

### Installation

There are just three steps. You'll need to open a terminal for this part, but
it's all copy-paste.

#### Step 1: Install the required libraries

These are small packages your system needs in order to build the program.

**Fedora:**

```bash
sudo dnf install gcc make pkg-config libmicrohttpd-devel libvirt-devel jansson-devel
```

**Ubuntu / Debian:**

```bash
sudo apt-get install build-essential pkg-config libmicrohttpd-dev libvirt-dev libjansson-dev
```

> 💡 **What does this do?** It installs a few small software libraries that QSWM
> needs. You only have to do this once.

#### Step 2: Build and install the program

Open a terminal, go to the project folder, and run:

```bash
make
sudo make install
```

> 💡 **What does this do?** `make` compiles the source code into a program your
> computer can run. `sudo make install` copies it to a system location so you
> can run it from anywhere.

#### Step 3: Start the server

**Option A — Run it as a system service (recommended):**

```bash
sudo systemctl enable --now qemu-snapshot-web-manager
```

This starts the server immediately and makes it start automatically every time
you reboot.

**Option B — Run it manually:**

```bash
sudo ./build/qswm
```

> ⚠️ **Why `sudo`?** Managing virtual machines requires administrator access.
> Without it, the tool can't talk to your VMs.

#### Step 4: Open your browser

Go to:

```
http://localhost:9091
```

That's it — you should see the QSWM interface with your virtual machines listed.
It's that simple!

<!-- Screenshot: Main interface showing all three panels with a VM selected -->

---

## 3. The Interface

The interface is split into three panels, side by side. Here's what each one
does:

<!-- Screenshot: Overview of the 3-panel layout -->

```
┌──────────────────────────────────────────────────────────────────┐
│  🖥️  QEMU Snapshot Manager                                       │
├────────────────┬─────────────────────────┬───────────────────────┤
│                │                         │                       │
│  Your Virtual  │    Snapshot Tree        │  Details & Actions    │
│  Machines      │                         │                       │
│                │    (visual diagram)     │  Snapshot info        │
│  • VM 1  🟢    │                         │  Action buttons       │
│  • VM 2  ⚫    │                         │                       │
│  • VM 3  🟡    │                         │  ─────────────────    │
│                │                         │  Shared Folders       │
│                │                         │                       │
└────────────────┴─────────────────────────┴───────────────────────┘
```

### Left Panel — Your Virtual Machines

This panel lists every virtual machine on your system. Each VM shows:

- Its **name**
- A **colored status badge**:
  - 🟢 **Green** = Running (the VM is on and active)
  - ⚫ **Gray** = Shut off (the VM is powered down)
  - 🟡 **Yellow** = Paused (the VM is frozen in place, like pressing pause)
  - 🔴 **Red** = Crashed (something went wrong)
- Its **vCPU count** and **memory** (e.g., "2 vCPUs · 4096 MB")

**Click on a VM** to select it. This loads its snapshot tree in the center panel
and its details on the right.

There's also a **refresh button** (↻) at the top if you've just started or
stopped a VM and the list hasn't updated yet.

<!-- Screenshot: Left panel showing a list of VMs with different status badges -->

### Center Panel — Snapshot Tree

This is the main visual area. It shows all your snapshots as a **tree diagram**,
with lines connecting parent snapshots to their children.

- **Green circle** = Your current position (where the VM is right now)
- **Gray circles** = Other snapshots you can go back to
- **Dashed border** = An external snapshot (more on that later)
- **Blue circle** = The snapshot you've clicked on / selected

You can interact with the tree:

- **Click** a snapshot to see its details in the right panel
- **Scroll** the mouse wheel to zoom in and out
- **Click and drag** on empty space to pan around
- **Double-click** on empty space to reset the view (zoom to fit)
- **Hover** over a snapshot to see a quick tooltip with its name, date, and type

If there are no snapshots yet, you'll see a friendly message inviting you to
create your first one.

<!-- Screenshot: Snapshot tree showing several connected snapshots with the current one highlighted green -->

### Right Panel — Details & Actions

When you select a snapshot from the tree, this panel shows:

- **Snapshot name** (as a heading)
- **Type badge**: "Internal" (blue) or "External" (purple)
- **Date created**
- **Description** (if you wrote one when creating the snapshot)
- Whether it's the **current** snapshot (marked with a ☆ star)

Below the details, you'll find **action buttons** — these let you revert to,
delete, or merge the snapshot. We'll cover those in detail in
[Section 5](#5-working-with-snapshots).

At the very bottom of this panel, you'll see the **Shared Folders** section,
which lists any folders shared between your host computer and the selected VM.
Each shared folder shows a **mount status badge** — a green "✓ Mounted" badge
if the folder is actively accessible inside the VM, or a gray "Not Mounted"
badge if it isn't. You'll also find **Mount** and **Unmount** buttons next to
each folder, and an **"+ Add Shared Folder"** button that includes a handy
directory browser for picking folders on your host computer.

<!-- Screenshot: Right panel showing snapshot details with action buttons and shared folders -->

---

## 4. Managing Your VMs

You can control your virtual machines directly from the interface. Select a VM
from the left panel, and you'll see control buttons.

### Starting a VM

1. Click the VM in the left panel.
2. Click **Start**.
3. The status badge will turn green once the VM is running.

> 💡 **What does "running" mean?** The VM is powered on and active — just like
> turning on a computer. You can connect to it with a viewer like `virt-manager`
> or a VNC client.

### Stopping a VM

1. Select the VM.
2. Click **Stop**.
3. The status badge will turn gray.

> 💡 **What does "shut off" mean?** The VM is powered down, like turning off a
> computer. Nothing is running, but all your files are still saved on the
> virtual disk.

### Pausing a VM

1. Select the VM.
2. Click **Pause**.
3. The status badge will turn yellow.

> 💡 **What does "paused" mean?** The VM is frozen in time. Everything stops
> exactly where it is — programs, downloads, everything. When you start it
> again, it picks up right where it left off. Think of it like pressing pause
> on a movie.

### Resuming a VM

If your VM is paused, you can unpause it:

1. Select the paused VM (it will have a yellow status badge).
2. Click **▶ Resume**.
3. The VM picks up exactly where it left off — the status badge turns green.

### Force Stopping a VM

Sometimes a VM won't respond to a normal **Stop**. In that case, you can
force it off:

1. Select the running or paused VM.
2. Click **⚡ Force**.
3. The VM is immediately powered off.

> ⚠️ **Use with caution!** Force Stop is like pulling the power plug on a real
> computer. Only use it when the normal **Stop** button doesn't work. Any
> unsaved work inside the VM will be lost.

### Auto-Refresh

The VM list automatically refreshes every **10 seconds**. This means if you
shut down a VM from inside the guest (e.g., clicking "Shut Down" in the VM's
desktop), the status badge will update on its own — no need to manually
refresh the page.

---

## 5. Working with Snapshots

This is the heart of the tool. Here's how to create, view, restore, delete, and
merge snapshots.

### Creating a Snapshot

1. Select your VM from the left panel.
2. Click the **"+ New Snapshot"** button at the top of the snapshot tree area.
3. A form will appear. Fill in:
   - **Name** (required): Give it a meaningful name, like
     `before-system-update` or `clean-install`.
   - **Description** (optional): A note to remind yourself what this snapshot is
     for.
   - **Type**: Choose from the dropdown:
     - **Internal (memory + disk state)** — Saves everything inside one file.
       Simple, safe, and the best choice for most users.
     - **External (disk-only, requires merge)** — Creates a separate file for
       the changes. More advanced. Can be done while the VM is running without
       pausing it.
4. Click **Create**.

Your new snapshot will appear in the tree, connected to the previous snapshot.

<!-- Screenshot: Create snapshot form with name, description, and type fields -->

> 💡 **Which type should I pick?** If you're not sure, choose **Internal**. It
> keeps things simple — everything is stored in one place and you don't have to
> worry about merging later.

### Editing a Snapshot

After creating a snapshot, you can change its description:

1. Select the snapshot in the tree.
2. In the detail panel, click the **✏️ Edit** button.
3. A text area appears with the current description.
4. Type your new description and click **💾 Save**, or click **✗ Cancel** to discard changes.

> **Note:** Snapshot names cannot be changed — this is a libvirt limitation. Only the description can be edited.

### Viewing the Snapshot Tree

The tree in the center panel shows the history of your snapshots and how they
relate to each other.

- Each **circle** is a snapshot.
- **Lines** between circles show parent-child relationships (which snapshot came
  from which).
- The **current/active snapshot** — the one your VM is "on" right now — has a
  **★ star** symbol above it and a **green glow** effect. This makes it easy to
  spot at a glance.
- **Gray circles** are older snapshots you can go back to.
- **Dashed borders** indicate external snapshots.

You can navigate the tree:

| Action | How |
|---|---|
| Select a snapshot | Click on it |
| Zoom in / out | Mouse scroll wheel |
| Pan around | Click and drag on empty space |
| Reset the view | Double-click on empty space |
| Quick info | Hover over a snapshot |

<!-- Screenshot: Snapshot tree with annotations pointing out green (current), gray (other), and dashed (external) nodes -->

#### Switching tree labels

By default, each snapshot node shows the creation date beneath its name. Click the **📅 Date** button in the toolbar above the tree to switch to showing descriptions instead. The button changes to **📝 Description** — click again to switch back. Your preference is remembered across page reloads.

### Reverting to a Snapshot

Reverting means "go back in time" to a previous save point.

1. Click on a snapshot in the tree.
2. In the right panel, click **"↩ Revert"**.
3. A confirmation dialog will appear showing the snapshot's details. Click the
   revert button to confirm.

Your VM will be restored to exactly the state it was in when that snapshot was
taken.

**What happens after reverting:**

- **Snapshot taken while VM was shut off:** The VM stays shut off at that
  snapshot's disk state. Start it manually when ready.
- **Snapshot taken while VM was running:** The VM **automatically resumes** in
  the exact running state — same open windows, same running programs. This is
  because the snapshot captured the VM's memory along with the disk.
- **Snapshot taken while VM was paused:** Same as running — the VM resumes in
  paused state with memory restored.

> ⚠️ **Warning:** Reverting will discard your VM's current state. Any unsaved
> work inside the VM — open documents, downloads in progress, anything not in a
> snapshot — will be lost. Make sure to save your work or create a new snapshot
> first if you want to keep your current state.

> 💡 **Note:** You cannot revert while the VM is running. The confirmation
> dialog will indicate when the VM has unsaved changes. If the VM is paused,
> you can revert directly without needing to shut down first.

#### What if you have unsaved changes?

If your VM's current state has changed since the last snapshot (e.g., you installed software or modified files), QSWM detects this and shows you two options:

- **💾 Save & Revert** — Automatically creates a backup snapshot (named `pre-revert-YYYYMMDD-HHMMSS`) before reverting. This way you can always go back if needed.
- **↩ Revert Without Saving** — Discards all changes since the target snapshot. This cannot be undone.

If your VM state matches the current snapshot exactly, the revert happens immediately without this prompt.

<!-- Screenshot: Revert confirmation dialog -->

### Deleting a Snapshot

1. Click on a snapshot in the tree.
2. In the right panel, click **"🗑 Delete"**.
3. Confirm the action.

The snapshot will be removed from the tree.

> ⚠️ **Important:** The VM must be **shut off** before deleting snapshots. If
> the VM is running or paused, delete will be blocked with a message asking you
> to shut down first. This ensures the snapshot data is fully removed from the
> disk file — not just the metadata.

> 💡 **Don't worry** — deleting a snapshot does **not** affect your VM's current
> state. It just removes that save point. Your VM keeps running (or stays off)
> exactly as it was. Think of it like erasing a save file in a game — the game
> itself isn't affected.

<!-- Screenshot: Delete confirmation dialog -->

### Orphaned Snapshots

Sometimes snapshot data can be left behind in disk files even after deletion
(for example, if snapshots were deleted using external tools while the VM was
running). These are called **orphaned snapshots**.

QSWM automatically scans for orphans:
- When you select a VM (if it's shut off)
- Every 5 minutes while the page is open
- Whenever the VM state changes (e.g., after shutting down)

> **Note:** Orphan scanning only runs when the VM is shut off — it needs direct access to the disk image files, which isn't possible while the VM is running.

If orphans are found, a **yellow warning banner** appears above the snapshot
tree showing how many were found. You can expand the list to see their names.

**To clean up orphaned snapshots:**

1. Make sure the VM is **shut off**.
2. Click the **"🧹 Clean Up"** button in the warning banner.
3. Confirm the action.

The orphaned data will be removed from the disk files, freeing up space and
preventing naming conflicts.

> 💡 **Why does this happen?** When QEMU is actively using a disk file (VM is
> running), it locks the file. Deleting a snapshot in this state can only remove
> the metadata — the actual data stays in the qcow2 file until the VM is shut
> off. QSWM prevents this by blocking deletes while the VM is running, but
> orphans can still occur from external tools like `virsh`.

### Merging External Snapshots

This option only appears for **external snapshots** (the ones with a dashed
border in the tree).

1. Click on an external snapshot in the tree.
2. In the right panel, click **"⊕ Merge"**.
3. Confirm the action.

> 💡 **What does merging do?** When you create external snapshots, each one
> creates a new file that stores just the changes since the last snapshot. Over
> time, you can end up with a chain of these files. Merging combines a
> snapshot's changes back into the main disk file. This simplifies the snapshot
> chain and can free up disk space.
>
> Think of it like this: instead of having a notebook full of sticky-note edits,
> merging writes all the edits neatly into the notebook itself.

> ⚠️ **Important:** The VM should be **shut off** before merging. If the VM is
> running, the merge may fail. Stop the VM first, then merge.

<!-- Screenshot: Merge button visible on an external snapshot's detail panel -->

### NVRAM Conversion for UEFI VMs

If your VM uses **UEFI firmware** (most modern VMs do), you might see an error
about "pflash" or "QCOW2 NVRAM" when trying to create an internal snapshot
(the kind that saves memory + disk together). This happens because the NVRAM
firmware file is in a format that doesn't support snapshots.

**How to fix it (one-time setup):**

1. **Stop** the VM first (it must be fully shut off).
2. Look at the error message — it will include a **"🔧 Convert NVRAM"** button.
   Click it.
3. Wait for the conversion to finish (it only takes a moment).
4. **Start** the VM again and try creating your snapshot — it will work now!

> 💡 **Good news:** You only need to do this once per VM. After the NVRAM file
> is converted, internal snapshots will work every time.

---

## 6. Shared Folders

### What are shared folders?

**Shared folders** let your virtual machine access files that live on your real
(host) computer. It's like a bridge between the two — you put files in a folder
on your host, and the VM can see them too.

### How they appear in QSWM

The tool automatically detects shared folders that use **virtiofs** (a fast,
modern file-sharing technology) or **9p** (an older but widely compatible
protocol). You'll find them in the **right panel** under the "Shared Folders"
heading, below the snapshot details.

For each shared folder, you'll see:

- **Mount tag** — The name the VM uses to find the folder (like a label).
- **Source path** — The actual folder location on your host computer
  (e.g., `/home/you/shared`).
- **Filesystem type** — Either "virtiofs" (fast, modern) or "9p" (compatible,
  older).
- **Mount status badge** — A green "✓ Mounted" badge if the folder is actively
  mounted inside the VM, or a gray "Not Mounted" badge if it isn't. The status
  is detected automatically in real-time when the guest agent is available.

<!-- Screenshot: Shared folders section in the right panel showing mount tags, paths, and status badges -->

### Adding a Shared Folder

You can add shared folders directly from the web interface:

1. In the **Shared Folders** section (right panel), click **"+ Add Shared Folder"**.
2. Fill in the form:
   - **Source Directory** — The folder on your host computer you want to share.
     You can type a path directly, or click the **"Browse..."** button to open
     a **directory browser modal**. The browser shows folders on your computer —
     click a folder to navigate into it, and click **"Select"** to choose it.
   - **Mount Tag** — A label the VM uses to identify this folder. One is
     auto-generated from the directory name (e.g., selecting
     `/home/user/shared` generates tag `shared`), but you can change it.
   - **Filesystem Type** — Choose **virtiofs** (the default) for the best
     performance, or **9p** if virtiofs doesn't work on your system.
3. Click **Add**.

The shared folder will be added to the VM's configuration. The VM may need to be restarted for it to take effect.

### Removing a Shared Folder

To remove a shared folder, click the **"Detach"** button next to it.

> ⚠️ **"Detach" does NOT delete your files.** It only removes the shared folder link from the VM configuration. Your files on the host computer are completely safe and untouched.

### Mounting and Unmounting Inside the VM

Once a shared folder is configured, you need to **mount** it inside the VM to actually access the files. QSWM can do this for you automatically if the VM has the **QEMU Guest Agent** installed.

- Click **"Mount"** next to a shared folder to mount it inside the VM at `/media/<tag>`.
- Click **"Unmount"** to disconnect it.

The buttons are smart — the **Mount** button is disabled (grayed out) when the
folder is already mounted, so you can't accidentally double-mount. Likewise,
**Unmount** is disabled when the folder isn't mounted. After each operation, the
mount status badge updates automatically to reflect the current state.

If the guest agent is not installed, the UI will show you the manual mount commands you can run inside the VM yourself.

> 💡 **What is the QEMU Guest Agent?** It's a small helper program that runs inside your VM. It lets the host computer send commands to the VM (like "mount this folder"). Without it, the host has no way to reach inside the VM. See [Installing the Guest Agent](#installing-the-qemu-guest-agent) below.

### Installing the QEMU Guest Agent

To use the Mount/Unmount buttons, install the guest agent inside your VM:

**Fedora / RHEL:**
```bash
sudo dnf install qemu-guest-agent
sudo systemctl enable --now qemu-guest-agent
```

**Ubuntu / Debian:**
```bash
sudo apt install qemu-guest-agent
sudo systemctl enable --now qemu-guest-agent
```

After installing, restart the VM or the agent service, and the Mount/Unmount buttons should work.

> 💡 **Tip:** If you try to mount a folder without the guest agent installed, QSWM shows a helpful error with installation instructions specific to your guest OS — Fedora, Ubuntu, Arch, Alpine, Windows, FreeBSD, and more. Just follow the on-screen steps.

### Auto-Mount (VirtioFS only)

Tired of clicking Mount every time you start your VM? The **Auto-Mount** feature
installs a tiny background service inside the guest that automatically discovers
and mounts VirtioFS shared folders for you — no clicks needed after setup.

#### What it does

When you click the setup button, QSWM detects your guest operating system and installs the appropriate background service:

| Guest OS | Service Type | Check Interval |
|----------|-------------|----------------|
| Linux (Fedora, Ubuntu, etc.) | systemd timer | Every 5 seconds |
| Linux (Alpine, Gentoo) | OpenRC init script + cron | Every 1 minute |
| Windows | PowerShell script + Scheduled Task | Every 5 minutes |
| FreeBSD | rc.d service + cron | Every 1 minute |
| macOS | launchd plist | Every 5 seconds |

The service discovers VirtioFS shared folders and mounts them automatically to `/media/<tag>` (Linux/FreeBSD/macOS) or a drive letter (Windows).

#### How to set it up

1. Make sure the VM is **running** and the **QEMU Guest Agent** is installed
   (see [Installing the Guest Agent](#installing-the-qemu-guest-agent)).
2. In the **Shared Folders** section (right panel), click the
   **"⚙️ Setup Auto-Mount"** button.
3. A confirmation dialog will appear — click **OK** to proceed.
4. That's it! Once installed, the button disappears and is replaced by a green
   **"✓ Auto-Mount Active"** badge.

This is a **one-time setup per VM**. The service persists across reboots — once
installed, your VirtioFS shares will be mounted automatically every time the VM
starts.

#### Good to know

- **VirtioFS only.** Auto-Mount discovers shares via `/sys/fs/virtiofs/`, which
  is a VirtioFS-specific interface. It does not work with 9p shares. You can
  still mount 9p shares manually using the Mount button.
- **Requires the Guest Agent.** The setup button uses the QEMU Guest Agent to
  write files and run commands inside the VM. If the agent isn't installed, the
  button won't appear.
- **Mount path is `/media/<tag>`.** Shares are mounted at `/media/` followed by
  the mount tag (e.g., a share tagged `projects` appears at `/media/projects`).
- **Status indicator.** Once installed, the setup button changes to a green badge: **✓ Auto-Mount Active**. This confirms the service is running inside the guest.

### SELinux Note (Fedora / RHEL)

On Fedora or RHEL systems, SELinux may block the guest agent from executing mount commands. If mount fails with a permission error, run this inside the VM:

```bash
sudo semanage permissive -a virt_qemu_ga_t
```

This tells SELinux to allow the guest agent to perform system operations like mounting folders.

> 💡 If `semanage` is not installed, install it with:
> - **Fedora / RHEL:** `sudo dnf install policycoreutils-python-utils`
> - **Ubuntu / Debian:** `sudo apt install policycoreutils-python-utils`

### Guest Sudoers Configuration

If you're using the project's guest dotfiles, the sudoers file at
`/etc/sudoers.d/qswm-automation` grants passwordless access to **specific
commands only** (not full root access):

- `mount`, `umount` — for mounting/unmounting shared folders
- `mkdir` — for creating mount-point directories
- `systemctl` — for enabling the auto-mount timer
- `findmnt` — for detecting mount status

This follows the principle of least privilege — the guest agent can perform the
operations it needs without having unrestricted root access.

---

## 7. Tips & Best Practices

✅ **Take snapshots before big changes.** About to install a major software
update? Upgrade your operating system? Try something risky? Create a snapshot
first. If anything goes wrong, you can jump right back.

✅ **Use meaningful names.** Name your snapshots something descriptive like
`before-upgrade-to-fedora-44` or `clean-install-with-dev-tools` instead of
`snap1` or `test`. Future-you will thank present-you.

✅ **Stick with internal snapshots unless you need live snapshots.** Internal
snapshots are simpler — everything lives in one file, and you don't need to
worry about merging. Only use external snapshots if you need to take a snapshot
while the VM is actively running.

✅ **Clean up old snapshots.** Snapshots take up disk space. If you have save
points you'll never go back to, delete them to reclaim space.

✅ **Keep your snapshot tree shallow.** A long chain of snapshots on top of
snapshots can slow things down. Try to keep it simple — branch only when you
need to, and merge or delete branches you're done with.

✅ **Shut down the VM before merging.** External snapshot merges work best (and
most reliably) when the VM is powered off.

✅ **Install the guest agent for the best experience.** With the QEMU Guest
Agent, you can mount and unmount shared folders with one click, and the UI
shows real-time mount status.

✅ **Enable Auto-Mount for hands-free shared folders.** Once you set up
Auto-Mount, your VirtioFS shared folders are mounted automatically every time
the VM starts — no manual clicking required.

✅ **Use virtiofs for better performance.** If your system supports it, virtiofs
is significantly faster than 9p for shared folders.

---

## 8. Troubleshooting

> 💡 **About error messages:** When something goes wrong, QSWM shows you
> specific details about what happened — not just a generic error. Notification
> messages (both successes and errors) stay visible until you perform another
> action, so you have time to read them.

### "No virtual machines found"

The tool can't see any VMs. This usually means the libvirt service isn't
running.

**Fix:** Start libvirt:

```bash
sudo systemctl start libvirtd
```

To make it start automatically on boot:

```bash
sudo systemctl enable libvirtd
```

### "Failed to connect to libvirt"

The tool can't communicate with the libvirt service.

**Fix:** Make sure you're running the tool with `sudo`:

```bash
sudo ./build/qswm
```

Or if using the system service, check that it's running:

```bash
sudo systemctl status qemu-snapshot-web-manager
```

### The web page won't load

You go to `http://localhost:9091` and nothing shows up.

**Fix:**

1. Make sure the server is actually running:
   ```bash
   sudo systemctl status qemu-snapshot-web-manager
   ```
   Or, if running manually, check that the terminal is still open with `qswm`
   running.

2. Double-check the port. The default is **9091**. If you changed it with the
   `--port` option, use that port in your browser instead.

3. Make sure nothing else is using port 9091:
   ```bash
   sudo ss -tlnp | grep 9091
   ```

### Snapshot operations fail

Some snapshot operations require the VM to be in a certain state.

**Fix:**

- **Creating an internal snapshot**: The VM may need to be paused or shut off.
  Try shutting down the VM first.
- **"pflash" or "QCOW2 NVRAM" error**: Your UEFI VM's firmware file needs to
  be converted. Stop the VM, click the **"🔧 Convert NVRAM"** button in the
  error message, then start the VM and try again. See
  [NVRAM Conversion for UEFI VMs](#nvram-conversion-for-uefi-vms) for details.
- **Deleting**: The VM must be **shut off**. If you see a message about shutting
  down first, stop the VM, then delete the snapshot.
- **"already exists" error when creating**: This usually means an orphaned
  snapshot exists in the disk file. Look for the ⚠️ orphan warning banner above
  the tree and click **"🧹 Clean Up"** to fix it.
- **Reverting**: Works in most states, but if it fails, try stopping the VM
  first, then reverting.
- **Merging**: The VM **must** be shut off. Stop the VM, then try again.

### External snapshot merge fails

**Fix:**

1. Make sure the VM is **shut off** (not just paused — fully stopped).
2. Check that you have enough free disk space. Merging needs room to combine
   the files.
3. Check that the disk files haven't been moved or renamed outside of libvirt.

### Mount/Unmount button doesn't work

The Mount and Unmount buttons require the **QEMU Guest Agent** to be running inside the VM.

**Fix:**

1. Install the guest agent inside the VM (see [Section 6](#installing-the-qemu-guest-agent)).
2. Make sure the VM is running (guest agent only works when the VM is on).
3. On Fedora/RHEL, check if SELinux is blocking the agent (see [SELinux Note](#selinux-note-fedora--rhel)).

### The snapshot tree looks empty

If you select a VM and the tree area is blank:

- You might not have any snapshots yet! Click **"+ New Snapshot"** to create
  your first one.
- If you know you have snapshots, try clicking the refresh button or
  reloading the page.

### Shared folder shows "Not Mounted" even though I mounted it manually

QSWM detects mount status using the guest agent's `findmnt` command. Make sure
the guest agent is running inside the VM and the folder is mounted at
`/media/<tag>` (the path QSWM expects).

### Mount button is grayed out

This means the folder is already mounted — the button disables itself to prevent
double-mounting. If you need to remount, click **Unmount** first, then Mount.

### Filesystem type: which should I choose?

Use **virtiofs** (the default) for the best performance. It's fast and modern.
Only choose **9p** if virtiofs doesn't work on your system — 9p is slower but
works on a wider range of setups.

---

## 9. Glossary

| Term | What it means |
|---|---|
| **VM (Virtual Machine)** | A simulated computer running inside your real computer. It has its own operating system, files, and programs — but it all lives in a file on your host machine. |
| **Snapshot** | A save point for a VM. It captures the VM's state at a moment in time so you can return to it later. |
| **Internal snapshot** | A snapshot where everything (disk and memory) is saved inside a single file. Simple and self-contained. Best for most users. |
| **External snapshot** | A snapshot that creates a separate overlay file for disk changes only. Can be taken while the VM is running. Needs to be merged later to clean up. |
| **Merge** | The process of combining an external snapshot's changes back into the main disk file. Simplifies the file chain and can free up disk space. |
| **Revert** | Going back to a previous snapshot. Restores the VM to the exact state it was in when that snapshot was taken. |
| **virtiofs** | A fast file-sharing technology that lets a VM access folders on the host computer. |
| **libvirt** | The software layer that manages your VMs behind the scenes. Tools like `virt-manager` and QSWM talk to libvirt, and libvirt talks to QEMU. |
| **QEMU** | The emulator/virtualizer that actually runs your virtual machines. It does the heavy lifting of simulating hardware. |
| **KVM** | A Linux feature that lets QEMU run VMs much faster by using your processor's built-in virtualization support. QEMU + KVM together give you near-native performance. |
| **Host** | Your real, physical computer — the one running the VMs. |
| **Guest** | The virtual machine itself — the simulated computer running inside the host. |
| **QEMU Guest Agent** | A helper service running inside the VM that lets the host send commands to the guest (like mounting folders). Required for the Mount/Unmount feature. |
| **9p** | An older but widely compatible protocol for sharing folders between host and guest. Works on more systems but is slower than virtiofs. |
| **Mount** | Making a shared folder accessible inside the VM. Like plugging in a USB drive — the folder appears at a location (e.g., `/media/myshare`) where you can access it. |
| **Unmount** | Disconnecting a mounted shared folder inside the VM. Like safely ejecting a USB drive. |
| **Directory browser** | A built-in file picker in QSWM that lets you navigate folders on your host computer to select a shared folder source path. |
| **Mount tag** | A short label (like a name tag) that identifies a shared folder. The VM uses this tag to find and mount the folder. |
| **Mount status** | Shows whether a shared folder is currently accessible inside the VM. "✓ Mounted" means the folder is active and accessible. "Not Mounted" means it's configured but not yet connected. |
| **Detach** | Removes a shared folder configuration from a VM. Does NOT delete any files on the host. |
| **Auto-Mount** | A background service inside the guest VM that automatically discovers and mounts VirtioFS shared folders. Supports Linux (systemd/OpenRC), Windows (scheduled task), FreeBSD (rc.d), and macOS (launchd). Set it up once and forget about it. |

---

*Happy snapshotting!* 🎉
