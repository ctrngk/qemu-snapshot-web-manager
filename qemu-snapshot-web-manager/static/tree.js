// Snapshot tree visualization (D3.js)
// Interactive tree with zoom/pan, tooltips, and HTMX integration

window.currentVm = null;
var _lastTreeJson = null;      // cache for smart diff
var _selectedSnapId = null;    // preserve selection across refreshes

// ── Tooltip setup ──────────────────────────────────────────────────
var tooltip = d3.select('body').append('div')
    .attr('class', 'tree-tooltip')
    .style('position', 'absolute')
    .style('padding', '8px 12px')
    .style('background', '#1e293b')
    .style('color', '#f1f5f9')
    .style('border-radius', '6px')
    .style('font-size', '0.75rem')
    .style('pointer-events', 'none')
    .style('opacity', 0)
    .style('z-index', 1000)
    .style('max-width', '260px')
    .style('box-shadow', '0 4px 12px rgba(0,0,0,0.3)');

// ── Main entry point ───────────────────────────────────────────────
function loadSnapshotTree(vmName, forceRefresh) {
    // Reset cache when switching VMs
    if (vmName !== window.currentVm) {
        _lastTreeJson = null;
        _selectedSnapId = null;
        forceRefresh = true;
    }
    window.currentVm = vmName;
    var container = document.getElementById('snapshot-tree');

    // Only show loading placeholder on first load (not on poll refreshes)
    if (!_lastTreeJson || forceRefresh) {
        container.innerHTML = '<p class="placeholder">Loading snapshot tree…</p>';
    }

    fetch('/api/vms/' + vmName + '/snapshots')
        .then(function(r) {
            if (!r.ok) throw new Error('HTTP ' + r.status);
            return r.text();
        })
        .then(function(text) {
            // Smart diff: skip re-render if data hasn't changed
            if (text === _lastTreeJson && !forceRefresh) return;
            _lastTreeJson = text;

            var data = JSON.parse(text);
            if (!data || !data.id || data.id === 'empty') {
                showEmptyState(container);
                return;
            }
            renderTree(data);
        })
        .catch(function(err) {
            container.innerHTML =
                '<p class="placeholder" style="color:var(--color-danger)">' +
                'Error loading snapshots: ' + err.message + '</p>';
        });
}

// ── Empty state ────────────────────────────────────────────────────
function showEmptyState(container) {
    container.innerHTML =
        '<p class="placeholder" style="text-align:center;margin-top:3rem">' +
        'No snapshots. Click \'+ New Snapshot\' to create one.</p>';
}

// ── Tree rendering ─────────────────────────────────────────────────
function renderTree(data) {
    var container = document.getElementById('snapshot-tree');
    container.innerHTML = '';

    var width = container.clientWidth || 600;
    var height = container.clientHeight || 400;

    var nodeRadius = 18;
    var nodePadding = { top: 60, bottom: 60, left: 40, right: 40 };

    // Build hierarchy
    var root = d3.hierarchy(data);
    var nodeCount = root.descendants().length;
    var leafCount = root.leaves().length;

    // Size tree to content
    var treeWidth = Math.max(200, leafCount * 100);
    var treeHeight = Math.max(200, root.height * 120);

    var treeLayout = d3.tree().size([treeWidth, treeHeight]);
    treeLayout(root);

    // Create SVG
    var svg = d3.select(container).append('svg')
        .attr('width', width)
        .attr('height', height)
        .style('width', '100%')
        .style('height', '100%');

    // Zoom group
    var g = svg.append('g');

    // ── Zoom / Pan ─────────────────────────────────────────────────
    var zoom = d3.zoom()
        .scaleExtent([0.3, 3])
        .on('zoom', function(e) { g.attr('transform', e.transform); });
    svg.call(zoom);

    // Fit tree into viewport
    function fitToView(animate) {
        var pad = 40;
        var bounds = g.node().getBBox();
        if (bounds.width === 0 || bounds.height === 0) return;

        var fullWidth = width;
        var fullHeight = height;
        var bw = bounds.width + pad * 2;
        var bh = bounds.height + pad * 2;
        var scale = Math.min(fullWidth / bw, fullHeight / bh, 1.5);
        var tx = fullWidth / 2 - (bounds.x + bounds.width / 2) * scale;
        var ty = fullHeight / 2 - (bounds.y + bounds.height / 2) * scale;

        var t = d3.zoomIdentity.translate(tx, ty).scale(scale);
        if (animate) {
            svg.transition().duration(500).call(zoom.transform, t);
        } else {
            svg.call(zoom.transform, t);
        }
    }

    // Double-click resets zoom
    svg.on('dblclick.zoom', null);
    svg.on('dblclick', function() { fitToView(true); });

    // ── Links ──────────────────────────────────────────────────────
    // Arrow marker for current-state connector
    svg.append('defs').append('marker')
        .attr('id', 'arrow-unsaved')
        .attr('markerWidth', 8)
        .attr('markerHeight', 6)
        .attr('refX', 8)
        .attr('refY', 3)
        .attr('orient', 'auto')
        .append('polygon')
        .attr('points', '0 0, 8 3, 0 6')
        .attr('fill', 'var(--color-warning, #f59e0b)');

    // Regular links (solid)
    g.selectAll('.tree-link')
        .data(root.links().filter(function(l) {
            return l.target.data.type !== 'current-state';
        }))
        .enter()
        .append('path')
        .attr('class', 'tree-link')
        .attr('d', d3.linkVertical()
            .x(function(d) { return d.x; })
            .y(function(d) { return d.y; })
        );

    // Current-state links (dashed arrow when dirty, solid subtle when clean)
    var csLinks = g.selectAll('.tree-link-unsaved')
        .data(root.links().filter(function(l) {
            return l.target.data.type === 'current-state';
        }))
        .enter();

    csLinks.append('line')
        .attr('class', function(d) {
            return d.target.data.isDirty ? 'tree-link-unsaved' : 'tree-link-clean';
        })
        .attr('x1', function(d) { return d.source.x; })
        .attr('y1', function(d) { return d.source.y + nodeRadius; })
        .attr('x2', function(d) { return d.target.x; })
        .attr('y2', function(d) { return d.target.y - nodeRadius * 0.7 - 2; })
        .attr('stroke', function(d) {
            return d.target.data.isDirty
                ? 'var(--color-warning, #f59e0b)'
                : 'var(--color-success, #22c55e)';
        })
        .attr('stroke-width', 2)
        .attr('stroke-dasharray', function(d) {
            return d.target.data.isDirty ? '6,4' : null;
        })
        .attr('marker-end', function(d) {
            return d.target.data.isDirty ? 'url(#arrow-unsaved)' : null;
        });

    // "unsaved changes" label — only shown when dirty
    var dirtyLinks = csLinks.filter(function(d) {
        return d.target.data.isDirty;
    });

    dirtyLinks.append('rect')
        .attr('class', 'unsaved-label-bg')
        .attr('x', function(d) { return (d.source.x + d.target.x) / 2 - 44; })
        .attr('y', function(d) { return (d.source.y + d.target.y) / 2 - 8; })
        .attr('width', 88)
        .attr('height', 15)
        .attr('rx', 3)
        .attr('fill', 'var(--bg-primary, #0f172a)')
        .attr('opacity', 0.9);

    dirtyLinks.append('text')
        .attr('class', 'unsaved-label-text')
        .attr('x', function(d) { return (d.source.x + d.target.x) / 2; })
        .attr('y', function(d) { return (d.source.y + d.target.y) / 2 + 4; })
        .attr('text-anchor', 'middle')
        .attr('fill', 'var(--color-warning, #f59e0b)')
        .attr('font-size', '0.6rem')
        .attr('font-weight', 'bold')
        .text('unsaved changes');

    // ── Nodes ──────────────────────────────────────────────────────
    var nodes = g.selectAll('.node')
        .data(root.descendants())
        .enter()
        .append('g')
        .attr('class', 'node')
        .attr('transform', function(d) {
            return 'translate(' + d.x + ',' + d.y + ')';
        });

    // Circles
    nodes.append('circle')
        .attr('r', function(d) {
            return d.data.type === 'current-state' ? nodeRadius * 0.7 : nodeRadius;
        })
        .attr('class', function(d) {
            var cls = 'node-circle';
            if (d.data.isCurrent) cls += ' current';
            if (d.data.type === 'current-state') {
                cls += d.data.isDirty ? ' current-state dirty' : ' current-state clean';
            }
            return cls;
        })
        .style('stroke-dasharray', function(d) {
            if (d.data.type === 'current-state') return '4,3';
            return d.data.type === 'external' ? '4,3' : null;
        })
        .on('click', function(event, d) {
            onNodeClick(event, d);
        })
        .on('mouseover', function(event, d) {
            showTooltip(event, d);
        })
        .on('mousemove', function(event) {
            tooltip
                .style('left', (event.pageX + 14) + 'px')
                .style('top', (event.pageY - 14) + 'px');
        })
        .on('mouseout', function() {
            tooltip.transition().duration(200).style('opacity', 0);
        });

    // Dot center for current-state node (green=clean, orange=dirty)
    nodes.filter(function(d) { return d.data.type === 'current-state'; })
        .append('circle')
        .attr('r', 4)
        .attr('fill', function(d) {
            return d.data.isDirty
                ? 'var(--color-warning, #f59e0b)'
                : 'var(--color-success, #22c55e)';
        })
        .attr('class', 'current-state-dot');

    // Labels below node
    nodes.append('text')
        .attr('class', function(d) {
            return d.data.type === 'current-state' ? 'node-label current-state-label' : 'node-label';
        })
        .attr('dy', function(d) {
            return d.data.type === 'current-state' ? (nodeRadius * 0.7 + 14) : (nodeRadius + 14);
        })
        .attr('text-anchor', 'middle')
        .style('fill', function(d) {
            if (d.data.type === 'current-state') {
                return d.data.isDirty
                    ? 'var(--color-warning, #f59e0b)'
                    : 'var(--color-success, #22c55e)';
            }
            return null;
        })
        .text(function(d) {
            if (d.data.type === 'current-state') {
                return 'Current State';
            }
            return d.data.name;
        });

    // VM state label below "Current State" node
    nodes.filter(function(d) { return d.data.type === 'current-state'; })
        .append('text')
        .attr('class', 'current-state-info')
        .attr('dy', nodeRadius * 0.7 + 28)
        .attr('text-anchor', 'middle')
        .style('fill', function(d) {
            return d.data.isDirty
                ? 'var(--color-warning, #f59e0b)'
                : 'var(--color-success, #22c55e)';
        })
        .text(function(d) { return d.data.description || ''; });

    // ★ indicator above current snapshot
    nodes.filter(function(d) { return d.data.isCurrent; })
        .append('text')
        .attr('class', 'node-current-star')
        .attr('dy', -(nodeRadius + 4))
        .attr('text-anchor', 'middle')
        .text('★');

    // Initial fit
    fitToView(false);

    // Restore selection after re-render (for smart refresh)
    if (_selectedSnapId) {
        d3.selectAll('.node-circle').each(function(d) {
            if (d.data.id === _selectedSnapId) {
                d3.select(this).classed('selected', true);
            }
        });
    }
}

// ── Node click handler ─────────────────────────────────────────────
function onNodeClick(event, snap) {
    event.stopPropagation();

    /* Ignore clicks on the virtual "Current State" node */
    if (snap.data.type === 'current-state') return;

    // Save selection for preservation across refreshes
    _selectedSnapId = snap.data.id;

    // Update selection styling
    d3.selectAll('.node-circle').classed('selected', false);
    d3.select(event.currentTarget).classed('selected', true);

    // Load snapshot detail via HTMX
    if (window.currentVm && snap.data.id) {
        htmx.ajax('GET',
            '/api/vms/' + window.currentVm + '/snapshots/' + snap.data.id,
            { target: '#snapshot-detail', swap: 'innerHTML' }
        );
    }
}

// ── Tooltip ────────────────────────────────────────────────────────
function showTooltip(event, d) {
    var html;
    if (d.data.type === 'current-state') {
        var stateColor = d.data.isDirty ? '#f59e0b' : '#22c55e';
        var stateLabel = d.data.isDirty ? 'Unsaved changes' : 'Clean — matches snapshot';
        html = '<strong>Current State</strong><br>' +
            '<span style="color:' + stateColor + '">' + (d.data.description || 'unknown') + '</span><br>' +
            '<em style="opacity:0.7">' + stateLabel + '</em>';
    } else {
        var desc = d.data.description || '';
        if (desc.length > 100) desc = desc.substring(0, 100) + '…';
        var dateStr = d.data.date ? new Date(d.data.date).toLocaleString() : '—';

        html =
            '<strong>' + d.data.name + '</strong><br>' +
            '<span style="opacity:0.8">' + dateStr + '</span><br>' +
            'Type: ' + (d.data.type || 'unknown') +
            (desc ? '<br><em>' + desc + '</em>' : '');
    }

    tooltip.html(html)
        .style('left', (event.pageX + 14) + 'px')
        .style('top', (event.pageY - 14) + 'px')
        .transition().duration(150).style('opacity', 1);
}

// ── Responsive resize (debounced) ──────────────────────────────────
var resizeTimer = null;
window.addEventListener('resize', function() {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(function() {
        if (window.currentVm) loadSnapshotTree(window.currentVm);
    }, 300);
});

// ── HTMX integration: refresh on VM state change ───────────────────
document.body.addEventListener('vmStateChanged', function() {
    if (window.currentVm) {
        _lastTreeJson = null;  // force re-render on explicit state change
        loadSnapshotTree(window.currentVm, true);
    }
    /* VM list refresh is handled by HTMX via hx-trigger="vmStateChanged from:body" */
});

// ── Auto-refresh: tree refreshes via vmStateChanged from VM list poll ──
// No separate timer needed — the VM list polls every 10s and fires
// vmStateChanged when content changes. Smart diff prevents flicker.
