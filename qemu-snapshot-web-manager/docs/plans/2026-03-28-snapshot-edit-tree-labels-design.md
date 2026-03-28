# Snapshot Edit & Tree Label Toggle — Design

## Problem

Users cannot edit snapshot descriptions after creation, and the snapshot tree only shows node names with no additional context (date or description).

## Features

### 1. Edit Snapshot Description

Users can edit snapshot descriptions from the detail panel.

**UI Flow:**
1. User clicks a snapshot node → detail panel shows snapshot info
2. An ✏️ Edit button appears next to the description
3. Clicking Edit replaces the description `<p>` with a `<textarea>` + Save/Cancel buttons
4. Save sends `PUT /api/vms/{vm}/snapshots/{snap}` with the new description
5. Response re-renders the detail panel and triggers tree refresh

**Backend:**
- New route: `PUT /api/vms/{vm}/snapshots/{snap}` with form field `description`
- New function `lv_edit_snapshot_description(vm_name, snap_name, new_description)`:
  1. Look up the snapshot via `virDomainSnapshotLookupByName()`
  2. Get current XML via `virDomainSnapshotGetXMLDesc()`
  3. Parse XML, replace `<description>` content
  4. Call `virDomainSnapshotCreateXML()` with `VIR_DOMAIN_SNAPSHOT_CREATE_REDEFINE` flag
  5. If the snapshot is the current one, also pass `VIR_DOMAIN_SNAPSHOT_CREATE_CURRENT`

**Note:** Snapshot names cannot be renamed — this is a libvirt limitation. Only description editing is supported, consistent with Virtual Machine Manager's behavior.

### 2. Tree Label Toggle

Each snapshot node in the D3.js tree shows a second line of text below the name.

**Display:**
- Line 1: Snapshot name (always shown)
- Line 2: Timestamp (default) or description (toggled)
- Second line uses smaller, muted font
- Timestamp format: raw ISO-8601 (same as detail panel, e.g., "2026-03-22T18:01:51Z")

**Toggle Control:**
- Small button in the tree toolbar area (next to "+ New Snapshot")
- Shows 📅 (date mode, default) or 📝 (description mode)
- Clicking toggles between date and description for all nodes
- State stored in `localStorage` so it persists across page reloads

## Files Changed

- `src/libvirt_backend.c` — New `lv_edit_snapshot_description()` function
- `src/libvirt_backend.h` — Declare new function
- `src/routes.c` — New PUT handler, route dispatcher entry
- `src/html_render.c` — Add Edit button + edit form to `render_snapshot_detail()`
- `static/tree.js` — Add second line labels, toggle state management
- `static/index.html` — Add toggle button in tree toolbar
- `static/styles.css` — Styles for edit form, second line labels, toggle button
