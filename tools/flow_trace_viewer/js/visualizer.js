/**
 * FlowTraceVisualizer - Generate Mermaid diagrams from flow trace events
 */
class FlowTraceVisualizer {
    constructor() {
        this.mermaid = null;
    }

    /**
     * Initialize Mermaid library
     */
    async init() {
        if (typeof mermaid !== 'undefined') {
            this.mermaid = mermaid;
            this.mermaid.initialize({
                startOnLoad: false,
                theme: 'default',
                sequence: {
                    diagramMarginX: 50,
                    diagramMarginY: 10,
                    actorMargin: 50,
                    width: 150,
                    height: 65,
                    boxMargin: 10,
                    boxTextMargin: 5,
                    noteMargin: 10,
                    messageMargin: 35
                },
                flowchart: {
                    htmlLabels: true,
                    curve: 'basis'
                }
            });
        }
    }

    /**
     * Generate sequence diagram from events
     * @param {object[]} events
     * @returns {string} Mermaid code
     */
    generateSequenceDiagram(events) {
        if (events.length === 0) {
            return 'sequenceDiagram\n  Note over NoEvents: No events to display';
        }

        // Build timeline with relative times
        const timeline = this.buildTimeline(events);

        // Extract unique participants
        const participants = new Set();
        for (const event of timeline) {
            participants.add(event.from);
            participants.add(event.to);
        }

        // Build Mermaid code
        let code = 'sequenceDiagram\n';
        code += '  autonumber\n';

        // Add participants
        for (const p of Array.from(participants).sort()) {
            const safeName = this.sanitizeName(p);
            code += `  participant ${safeName} as ${p}\n`;
        }

        // Add events
        let lastTime = 0;
        for (const event of timeline) {
            const from = this.sanitizeName(event.from);
            const to = this.sanitizeName(event.to);

            // Add note with timing if there's a gap
            if (event.relativeMs - lastTime > 100) {
                code += `  Note over ${from},${to}: +${event.relativeMs - lastTime}ms\n`;
            }
            lastTime = event.relativeMs;

            // Build message with event type
            let message = event.event;
            if (event.duration_us) {
                message += ` (${Math.round(event.duration_us / 1000)}ms)`;
            }

            // Arrow style based on event type
            const arrow = this.getArrowStyle(event.event);

            code += `  ${from}${arrow}${to}: ${message}\n`;

            // Add notes for inputs/outputs if present
            if (event.inputs) {
                const inputPreview = this.truncate(event.inputs, 50);
                code += `  Note right of ${from}: in: ${inputPreview}\n`;
            }
            if (event.outputs) {
                const outputPreview = this.truncate(event.outputs, 50);
                code += `  Note left of ${to}: out: ${outputPreview}\n`;
            }
        }

        return code;
    }

    /**
     * Generate flow diagram from events
     * @param {object[]} events
     * @returns {string} Mermaid code
     */
    generateFlowDiagram(events) {
        if (events.length === 0) {
            return 'flowchart LR\n  NoEvents[No events to display]';
        }

        // Track all nodes and edges
        const nodes = new Map(); // name -> {label, count}
        const edges = new Map(); // "from->to:event" -> {count, totalDuration}

        for (const event of events) {
            // Track nodes
            if (!nodes.has(event.from)) {
                nodes.set(event.from, { label: event.from, count: 0 });
            }
            nodes.get(event.from).count++;

            if (!nodes.has(event.to)) {
                nodes.set(event.to, { label: event.to, count: 0 });
            }
            nodes.get(event.to).count++;

            // Track edges
            const edgeKey = `${event.from}->${event.to}:${event.event}`;
            if (!edges.has(edgeKey)) {
                edges.set(edgeKey, { from: event.from, to: event.to, event: event.event, count: 0, totalDuration: 0 });
            }
            const edge = edges.get(edgeKey);
            edge.count++;
            if (event.duration_us) {
                edge.totalDuration += event.duration_us;
            }
        }

        // Build Mermaid code
        let code = 'flowchart LR\n';

        // Add nodes
        for (const [name, data] of nodes) {
            const safeName = this.sanitizeId(name);
            code += `  ${safeName}["${name}<br/>(${data.count})"]\n`;
        }

        // Add edges
        for (const [key, edge] of edges) {
            const fromId = this.sanitizeId(edge.from);
            const toId = this.sanitizeId(edge.to);
            let label = edge.event;
            if (edge.count > 1) {
                label += ` (x${edge.count})`;
            }
            if (edge.totalDuration > 0) {
                label += `<br/>${Math.round(edge.totalDuration / 1000)}ms`;
            }
            code += `  ${fromId} -->|"${label}"| ${toId}\n`;
        }

        // Add styles
        code += '\n  %% Styles\n';
        code += '  classDef external fill:#f9f,stroke:#333,stroke-width:2px;\n';
        code += '  classDef dispatcher fill:#bbf,stroke:#333,stroke-width:2px;\n';
        code += '  classDef coordinator fill:#bfb,stroke:#333,stroke-width:2px;\n';
        code += '  classDef skill fill:#fbf,stroke:#333,stroke-width:2px;\n';

        // Apply styles based on node names
        for (const [name, data] of nodes) {
            const safeName = this.sanitizeId(name);
            if (name === 'external') {
                code += `  class ${safeName} external\n`;
            } else if (name.includes('dispatcher')) {
                code += `  class ${safeName} dispatcher\n`;
            } else if (name.includes('coordinator')) {
                code += `  class ${safeName} coordinator\n`;
            } else {
                code += `  class ${safeName} skill\n`;
            }
        }

        return code;
    }

    /**
     * Generate timing table HTML
     * @param {object[]} events
     * @returns {string} HTML table
     */
    generateTimingTable(events) {
        if (events.length === 0) {
            return '<p>暂无事件可显示</p>';
        }

        const timeline = this.buildTimeline(events);

        let html = '<table class="timing-table">\n';
        html += '<thead><tr>';
        html += '<th>时间</th>';
        html += '<th>相对时间</th>';
        html += '<th>类型</th>';
        html += '<th>来源</th>';
        html += '<th>目标</th>';
        html += '<th>事件</th>';
        html += '<th>耗时</th>';
        html += '<th>输入</th>';
        html += '<th>输出</th>';
        html += '<th>标签</th>';
        html += '<th>扩展属性</th>';
        html += '<th>task_id</th>';
        html += '<th>trace_id</th>';
        html += '</tr></thead>\n';
        html += '<tbody>\n';

        for (const event of timeline) {
            const attrStr = this.formatAttrs(event.attrs);
            const inputStr = this.truncateCell(event.inputs, 80);
            const outputStr = this.truncateCell(event.outputs, 80);
            const labelStr = this.formatLabels(event.labels);
            html += '<tr>';
            html += `<td class="time">${event.time}</td>`;
            html += `<td class="relative">${event.relativeMs}ms</td>`;
            html += `<td class="type"><span class="badge type-${event.type}">${event.type}</span></td>`;
            html += `<td class="from">${event.from}</td>`;
            html += `<td class="to">${event.to}</td>`;
            html += `<td class="event">${event.event}</td>`;
            html += `<td class="duration">${event.duration_us ? Math.round(event.duration_us / 1000) + 'ms' : '-'}</td>`;
            html += `<td class="data-cell" title="${this.escapeHtml(event.inputs || '')}">${inputStr}</td>`;
            html += `<td class="data-cell" title="${this.escapeHtml(event.outputs || '')}">${outputStr}</td>`;
            html += `<td class="labels-cell">${labelStr}</td>`;
            html += `<td class="attrs-cell">${attrStr}</td>`;
            html += `<td class="id-cell">${event.task_id || '-'}</td>`;
            html += `<td class="id-cell">${event.trace_id || '-'}</td>`;
            html += '</tr>\n';
        }

        html += '</tbody></table>\n';
        return html;
    }

    /** Format labels as colored pills */
    formatLabels(labels) {
        if (!labels) return '-';
        // Labels format: "test_flow=e2e_basic|env=dev"
        const parts = labels.split('|');
        const pills = parts.map(part => {
            const trimmed = part.trim();
            return `<span class="label-pill">${this.escapeHtml(trimmed)}</span>`;
        });
        return pills.join(' ');
    }

    /** Format attrs map as compact key=value list */
    formatAttrs(attrs) {
        if (!attrs || typeof attrs !== 'object') return '-';
        const pairs = Object.entries(attrs).map(([k, v]) => `<span class="attr-pill">${k}=${v}</span>`);
        return pairs.length > 0 ? pairs.join(' ') : '-';
    }

    /** Truncate cell content for display */
    truncateCell(str, maxLen) {
        if (!str) return '-';
        const escaped = this.escapeHtml(str);
        return str.length > maxLen ? escaped.substring(0, maxLen) + '…' : escaped;
    }

    /** Escape HTML special chars */
    escapeHtml(str) {
        return String(str)
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;');
    }

    /**
     * Render Mermaid diagram
     * @param {string} containerId - Element ID to render into
     * @param {string} code - Mermaid code
     */
    async render(containerId, code) {
        if (!this.mermaid) {
            document.getElementById(containerId).innerHTML = `<pre>${code}</pre>`;
            return;
        }

        try {
            const { svg } = await this.mermaid.render('mermaid-svg', code);
            document.getElementById(containerId).innerHTML = svg;
        } catch (e) {
            console.error('Mermaid render error:', e);
            document.getElementById(containerId).innerHTML = `<pre class="error">${code}</pre><p class="error">Error: ${e.message}</p>`;
        }
    }

    // Helper methods

    buildTimeline(events) {
        if (events.length === 0) return [];

        const sorted = [...events].sort((a, b) => a.time.localeCompare(b.time));
        const baseline = this.parseTimestamp(sorted[0].time);

        return sorted.map(event => ({
            ...event,
            relativeMs: this.parseTimestamp(event.time) - baseline
        }));
    }

    parseTimestamp(timeStr) {
        const match = timeStr.match(/(\d{4}-\d{2}-\d{2}) (\d{2}):(\d{2}):(\d{2})\.(\d{3})/);
        if (!match) return 0;
        const [, datePart, hours, minutes, seconds, ms] = match;
        const date = new Date(`${datePart}T${hours}:${minutes}:${seconds}.${ms}`);
        return date.getTime();
    }

    sanitizeName(name) {
        return name.replace(/[^a-zA-Z0-9_]/g, '_').replace(/^[0-9]/, '_$&');
    }

    sanitizeId(name) {
        return 'n_' + name.replace(/[^a-zA-Z0-9_]/g, '_').replace(/^[0-9]/, '_$&');
    }

    getArrowStyle(eventType) {
        switch (eventType) {
            case 'response-sent':
            case 'workflow-completed':
            case 'node-completed':
            case 'skill-completed':
            case 'dialogue-completed':
                return '-->>';
            case 'data-exchange':
            case 'external-call':
                return '-->';
            case 'task-dispatched':
                return '->>+';
            default:
                return '->>';
        }
    }

    truncate(str, maxLen) {
        if (!str) return '';
        const escaped = str.replace(/"/g, "'").replace(/\n/g, ' ');
        return escaped.length > maxLen ? escaped.substring(0, maxLen) + '...' : escaped;
    }
}

// Export for use
if (typeof module !== 'undefined' && module.exports) {
    module.exports = FlowTraceVisualizer;
}