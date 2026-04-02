# Documentation Update Implementation Plan


**Goal:** Update all project documentation (README, User Guide, Developer Guide, Getting Started) to cover features added since the docs were last updated — cross-OS auto-mount, orphan scanner, guest agent help, snapshot editing, save & revert, tree label toggle, NVRAM conversion, resume/force-stop, dirty state tracking, SIGPIPE/thread-safety fixes.

**Architecture:** Four documentation files need surgical updates. Each task targets one file with specific section edits. No code changes — documentation only.

**Tech Stack:** Markdown, git

---

## Task 1: Update README.md — Features & API Reference

**Files:**
- Modify: `README.md`

**Step 1: Update Features section (line 9-22)**

Replace the features list. Changes:
- "VM lifecycle controls" → add resume, force-stop
- "Auto-mount" → mention cross-OS support (not just systemd)
- "Linux-focused" → "Cross-platform guest support" with Linux/Windows/macOS/FreeBSD
- Add: orphan snapshot detection & cleanup
- Add: snapshot description editing
- Add: NVRAM auto-conversion for UEFI VMs
- Add: dirty state tracking (warns about unsaved changes on revert)
- Add: OS-specific guest agent install help

New features list:
```markdown
## Features

- **Visual snapshot tree** — interactive D3.js tree visualization, similar to VirtualBox's snapshot view
- **Internal & external snapshot management** — create, delete, revert, and merge snapshots
- **Snapshot description editing** — modify snapshot descriptions after creation, toggle tree labels between dates and descriptions
- **Auto-merge for external snapshots** — block commit support to flatten snapshot chains
- **Orphan snapshot detection** — scans for leftover qcow2 internal snapshots after metadata-only deletes, with one-click cleanup
- **Smart revert** — detects unsaved VM state changes and offers "Save & Revert" (auto-creates backup snapshot) or "Revert Without Saving"
- **NVRAM auto-conversion** — automatically converts raw UEFI NVRAM files to qcow2 format when needed for internal snapshots
- **Shared folder management** — list (virtiofs + 9p), add, detach, mount/unmount via guest agent, real-time mount status detection, server-side directory browser for selecting source paths
- **Cross-OS auto-mount** — one-click setup detects guest OS and installs the appropriate background service: systemd timer (Linux), OpenRC init script (Alpine), scheduled task (Windows), rc.d service (FreeBSD), or launchd plist (macOS)
- **Guest agent integration** — mount and unmount shared folders inside VMs through QEMU guest agent, with OS-specific installation help when the agent is missing
- **VM lifecycle controls** — start, stop, pause, resume, and force-stop virtual machines
- **Real-time status updates** — keeps the UI in sync with VM state changes
- **Cross-platform guest support** — detects guest OS automatically; Linux host (libvirt/KVM) with experimental macOS host support (libvirt/HVF)
- **Zero JS toolchain** — no npm, no node, no build step — just C and a browser
- **Tiny footprint** — single ~100KB binary, 14KB of HTMX + D3 loaded from CDN
```

**Step 2: Update API Reference table (line 135-159)**

Add these 9 missing endpoints to the existing table, inserted in logical order:
- After `/api/vms/{name}/pause`: `POST /api/vms/{name}/resume` (Resume paused VM) and `POST /api/vms/{name}/force-stop` (Force-stop VM)
- After `/api/vms/{name}/snapshots/{snap}`: `GET /api/vms/{name}/snapshots/{snap}/edit` (Edit form HTML), `PUT /api/vms/{name}/snapshots/{snap}` (Update snapshot description), `GET /api/vms/{name}/snapshots/{snap}/revert-confirm` (Revert confirmation dialog)
- After the last snapshot endpoint: `POST /api/vms/{name}/convert-nvram` (Convert UEFI NVRAM to qcow2)
- After shared-folders section: `GET /api/vms/{name}/orphan-check` (Scan for orphan snapshots), `POST /api/vms/{name}/orphan-cleanup` (Remove orphan snapshots)

**Step 3: Update Known Limitations (line 160-168)**

Change "Guest agent required" line to note cross-OS support:
```markdown
- **Guest agent required** — shared folder mount/unmount and auto-mount require QEMU guest agent installed in the VM; the UI shows OS-specific install instructions when the agent is missing
```

**Step 4: Verify and commit**

```bash
cd /home/fedora/Documents/fedora43-qemu/qemu-snapshot-web-manager
# Quick visual check
head -25 README.md
git add README.md
git commit -m "docs: update README with all new features and API endpoints"
```

---

## Task 2: Update User Guide — Missing Features

**Files:**
- Modify: `docs/user-guide.md`

**Step 1: Add "Editing a Snapshot" subsection after "Creating a Snapshot" (after line ~320)**

Insert new subsection in Section 5:
```markdown
### Editing a Snapshot

After creating a snapshot, you can change its description:

1. Select the snapshot in the tree
2. In the detail panel, click the **✏️ Edit** button
3. A text area appears with the current description
4. Type your new description and click **💾 Save**, or click **✗ Cancel** to discard changes

> **Note:** Snapshot names cannot be changed — this is a libvirt limitation. Only the description can be edited.
```

**Step 2: Expand "Reverting to a Snapshot" (around line 348-380)**

After the existing revert instructions, add a new paragraph about the Save & Revert feature:
```markdown
#### What if you have unsaved changes?

If your VM's current state has changed since the last snapshot (e.g., you installed software or modified files), QSWM detects this and shows you two options:

- **💾 Save & Revert** — Automatically creates a backup snapshot (named `pre-revert-YYYYMMDD-HHMMSS`) before reverting. This way you can always go back if needed.
- **↩ Revert Without Saving** — Discards all changes since the target snapshot. This cannot be undone.

If your VM state matches the current snapshot exactly, the revert happens immediately without this prompt.
```

**Step 3: Add tree label toggle note in Section 3 "Center Panel" (after line ~200)**

Add after the tree interaction table:
```markdown
#### Switching tree labels

By default, each snapshot node shows the creation date beneath its name. Click the **📅 Date** button in the toolbar above the tree to switch to showing descriptions instead. The button changes to **📝 Description** — click again to switch back. Your preference is remembered across page reloads.
```

**Step 4: Expand "Auto-Mount" section for cross-OS support (around line 560-598)**

Replace the "What it does" paragraph to mention cross-OS:
```markdown
#### What it does

When you click **⚙ Install Auto-Mount**, QSWM detects your guest operating system and installs the appropriate background service:

| Guest OS | Service Type | Check Interval |
|----------|-------------|----------------|
| Linux (Fedora, Ubuntu, etc.) | systemd timer | Every 5 seconds |
| Linux (Alpine, Gentoo) | OpenRC init script + cron | Every 1 minute |
| Windows | PowerShell script + Scheduled Task | Every 5 minutes |
| FreeBSD | rc.d service + cron | Every 1 minute |
| macOS | launchd plist | Every 5 seconds |

The service discovers VirtioFS shared folders and mounts them automatically to `/media/<tag>` (Linux/FreeBSD/macOS) or a drive letter (Windows).
```

**Step 5: Add auto-mount status indicator note (after "Good to know" section ~line 588)**

```markdown
- Once the auto-mount service is installed, the button changes to a green badge: **✓ Auto-Mount Active**. This confirms the service is running inside the guest.
```

**Step 6: Add guest agent help note (after "Installing the QEMU Guest Agent" section ~line 560)**

```markdown
> **💡 Tip:** If you try to mount a folder without the guest agent installed, QSWM shows a helpful error with installation instructions specific to your guest OS — Fedora, Ubuntu, Arch, Alpine, Windows, FreeBSD, and more. Just follow the on-screen steps.
```

**Step 7: Clarify orphan scanner scope (Section 5 "Orphaned Snapshots" ~line 401)**

Add to the orphan scanner section:
```markdown
> **Note:** Orphan scanning only runs when the VM is shut off — it needs direct access to the disk image files, which isn't possible while the VM is running.
```

**Step 8: Update Glossary (end of file)**

Update the Auto-Mount definition:
```markdown
| **Auto-Mount** | A background service inside the guest VM that automatically discovers and mounts VirtioFS shared folders. Supports Linux (systemd/OpenRC), Windows (scheduled task), FreeBSD (rc.d), and macOS (launchd). |
```

**Step 9: Commit**

```bash
git add docs/user-guide.md
git commit -m "docs: update user guide with editing, save-revert, cross-OS auto-mount, label toggle"
```

---

## Task 3: Update Developer Guide — APIs & Architecture

**Files:**
- Modify: `docs/developer-guide.md`

**Step 1: Add libvirt-qemu to Key Libraries table (after line ~29)**

Add row to the libraries table:
```markdown
| libvirt-qemu | 10.x | Guest agent commands via `virDomainQemuAgentCommand()` |
```

**Step 2: Add libvirt-qemu-devel to Prerequisites (line ~84 Fedora, ~91 Ubuntu)**

Fedora section should include `libvirt-qemu` in the package list if not there already. Check and add if missing.

**Step 3: Add missing API routes to Section 6 (line ~478)**

Add 3 missing endpoint rows to the API reference table:
```markdown
| `GET`    | `/api/vms/{name}/snapshots/{snap}/edit`           | Edit snapshot form (HTML)              |
| `PUT`    | `/api/vms/{name}/snapshots/{snap}`                 | Update snapshot description             |
| `GET`    | `/api/vms/{name}/snapshots/{snap}/revert-confirm`  | Revert confirmation dialog (HTML)       |
```

Also verify these are listed (from README audit they may already be there):
- `POST /api/vms/{name}/resume`
- `POST /api/vms/{name}/force-stop`
- `POST /api/vms/{name}/convert-nvram`
- `GET /api/vms/{name}/orphan-check`
- `POST /api/vms/{name}/orphan-cleanup`

**Step 4: Add render functions to HTML Renderers section (line ~324)**

Add to the function list in Section 5 "HTML Renderers":
```markdown
| `render_error_html(html)` | Error alert that passes HTML through unescaped (for intentional HTML content like confirmation dialogs) |
| `render_guest_agent_help()` | Expandable OS-specific guest agent installation instructions (Fedora, Ubuntu, Arch, Alpine, Windows, FreeBSD) |
```

**Step 5: Expand Guest Agent Integration section (line ~601) with OS detection**

Add new subsection after "Auto-Mount System" (~line 685):
```markdown
### Guest OS Detection

The `detect_guest_os()` function identifies the guest operating system to install the correct auto-mount service. It uses a two-stage strategy:

1. **Primary:** Send `guest-get-osinfo` QGA command (available in QEMU GA 2.10+). Parses the JSON response for `name`, `id`, and `kernel-release` fields.
2. **Fallback:** If `guest-get-osinfo` is unavailable, probe for OS-specific binaries via `guest_exec()`:
   - `cmd.exe /c echo ok` → Windows
   - `freebsd-version` → FreeBSD
   - `sw_vers` → macOS
   - `systemctl --version` → Linux (systemd)
   - `rc-service --version` → Linux (OpenRC)

Returns a `guest_os_t` enum: `GUEST_OS_LINUX_SYSTEMD`, `GUEST_OS_LINUX_OPENRC`, `GUEST_OS_LINUX_OTHER`, `GUEST_OS_WINDOWS`, `GUEST_OS_FREEBSD`, `GUEST_OS_MACOS`, `GUEST_OS_UNKNOWN`.

Each OS type dispatches to a specific setup function:
- `setup_automount_systemd()` — systemd timer + service unit + bash script
- `setup_automount_openrc()` — init script + cron job + sh script
- `setup_automount_windows()` — PowerShell script + schtasks.exe scheduled task
- `setup_automount_freebsd()` — rc.d script + cron job + sh script
- `setup_automount_macos()` — launchd plist + bash script
```

**Step 6: Add SIGPIPE handling note to Architecture section (line ~154 area)**

Add to "Request Flow" or "Thread Safety" subsection:
```markdown
### Signal Handling

The server ignores `SIGPIPE` signals (`signal(SIGPIPE, SIG_IGN)` in `main.c`). Without this, the process crashes when writing a response to a client that has already closed the connection — a common occurrence with HTMX polling and browser tab closes.
```

**Step 7: Commit**

```bash
git add docs/developer-guide.md
git commit -m "docs: update dev guide with OS detection, missing APIs, signal handling"
```

---

## Task 4: Update Getting Started — Minor Additions

**Files:**
- Modify: `docs/getting-started.md`

**Step 1: Add cross-OS auto-mount mention (after line ~121 "Guest Agent" section)**

In the Guest Agent section, add a note:
```markdown
> **Cross-OS support:** Auto-mount works on Linux (systemd and OpenRC), Windows, FreeBSD, and macOS guests. QSWM auto-detects the guest OS and installs the right service.
```

**Step 2: Commit**

```bash
git add docs/getting-started.md
git commit -m "docs: add cross-OS auto-mount note to getting started guide"
```

---

## Task 5: Final Review & Verify

**Step 1: Check all docs render correctly**

```bash
# Verify no broken markdown
wc -l docs/*.md README.md
cat README.md | head -30
```

**Step 2: Verify all features from git log are now documented**

Cross-reference these commits against the updated docs:
- c573a42: cross-OS auto-mount → README ✓, User Guide ✓, Dev Guide ✓
- d9e2c15: guest agent install instructions → User Guide ✓
- a89842b: orphan snapshot scanner → README ✓ (already in User/Dev guide)
- 19abb26: HTML rendering fix → Dev Guide ✓ (render_error_html)
- 16d9090: server stability → Dev Guide ✓ (SIGPIPE, threading)

**Step 3: Final commit (if any remaining fixes)**

```bash
git add -A
git commit -m "docs: final review pass for documentation completeness"
```
