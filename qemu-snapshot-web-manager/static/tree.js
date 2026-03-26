// Snapshot tree visualization (D3.js)
// Interactive tree with zoom/pan, tooltips, and HTMX integration

window.currentVm = null;

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
function loadSnapshotTree(vmName) {
    window.currentVm = vmName;
    var container = document.getElementById('snapshot-tree');
    container.innerHTML = '<p class="placeholder">Loading snapshot tree…</p>';

    fetch('/api/vms/' + vmName + '/snapshots')
        .then(function(r) {
            if (!r.ok) throw new Error('HTTP ' + r.status);
            return r.json();
        })
        .then(function(data) {
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
    g.selectAll('.tree-link')
        .data(root.links())
        .enter()
        .append('path')
        .attr('class', 'tree-link')
        .attr('d', d3.linkVertical()
            .x(function(d) { return d.x; })
            .y(function(d) { return d.y; })
        );

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
        .attr('r', nodeRadius)
        .attr('class', function(d) {
            var cls = 'node-circle';
            if (d.data.isCurrent) cls += ' current';
            return cls;
        })
        .style('stroke-dasharray', function(d) {
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

    // Labels below node
    nodes.append('text')
        .attr('class', 'node-label')
        .attr('dy', nodeRadius + 14)
        .attr('text-anchor', 'middle')
        .text(function(d) { return d.data.name; });

    // Initial fit
    fitToView(false);
}

// ── Node click handler ─────────────────────────────────────────────
function onNodeClick(event, snap) {
    event.stopPropagation();

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
    var desc = d.data.description || '';
    if (desc.length > 100) desc = desc.substring(0, 100) + '…';
    var dateStr = d.data.date ? new Date(d.data.date).toLocaleString() : '—';

    var html =
        '<strong>' + d.data.name + '</strong><br>' +
        '<span style="opacity:0.8">' + dateStr + '</span><br>' +
        'Type: ' + (d.data.type || 'unknown') +
        (desc ? '<br><em>' + desc + '</em>' : '');

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
        loadSnapshotTree(window.currentVm);
    }
    /* VM list refresh is handled by HTMX via hx-trigger="vmStateChanged from:body" */
});
