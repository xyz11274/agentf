/**
 * FlowTraceParser - Parse and filter flow trace JSON logs
 */
class FlowTraceParser {
    constructor() {
        this.events = [];
    }

    /**
     * Parse JSON log content
     * @param {string} content - Raw log file content (JSON lines)
     * @returns {FlowTraceParser} this for chaining
     */
    parseFile(content) {
        this.events = [];
        const lines = content.split('\n');
        for (const line of lines) {
            const trimmed = line.trim();
            if (!trimmed || !trimmed.startsWith('{')) continue;
            try {
                const event = JSON.parse(trimmed);
                this.events.push(event);
            } catch (e) {
                console.warn('Failed to parse line:', trimmed, e);
            }
        }
        return this;
    }

    /**
     * Get all unique session IDs
     * @returns {string[]}
     */
    getSessions() {
        const sessions = new Set();
        for (const event of this.events) {
            if (event.session) {
                sessions.add(event.session);
            }
        }
        return Array.from(sessions).sort();
    }

    /**
     * Get all unique dialogue IDs for a session
     * @param {string} sessionId
     * @returns {string[]}
     */
    getDialogues(sessionId) {
        const dialogues = new Set();
        for (const event of this.events) {
            if (event.session === sessionId && event.dialogue) {
                dialogues.add(event.dialogue);
            }
        }
        return Array.from(dialogues).sort();
    }

    /**
     * Get all unique labels from events (full label strings)
     * @returns {string[]}
     */
    getLabels() {
        const labels = new Set();
        for (const event of this.events) {
            if (event.labels) {
                labels.add(event.labels);
            }
        }
        return Array.from(labels).sort();
    }

    /**
     * Filter events by multiple labels (OR logic - event matches any of the selected labels)
     * @param {string[]} selectedLabels - Array of label strings to filter by
     * @returns {object[]}
     */
    filterByLabels(selectedLabels) {
        if (!selectedLabels || selectedLabels.length === 0) {
            return this.events;
        }
        return this.events.filter(e => {
            if (!e.labels) return false;
            return selectedLabels.some(label => e.labels === label);
        });
    }

    /**
     * Filter events by session, dialogue, and labels combined
     * @param {string} sessionId
     * @param {string} dialogueId
     * @param {string[]} selectedLabels
     * @returns {object[]}
     */
    filterEvents(sessionId, dialogueId, selectedLabels) {
        return this.events.filter(e => {
            // Session filter
            if (sessionId && e.session !== sessionId) return false;
            // Dialogue filter
            if (dialogueId && e.dialogue !== dialogueId) return false;
            // Labels filter (OR logic)
            if (selectedLabels && selectedLabels.length > 0) {
                if (!e.labels) return false;
                if (!selectedLabels.some(label => e.labels === label)) return false;
            }
            return true;
        });
    }

    /**
     * Get total count of unique dialogues across all sessions
     * @returns {number}
     */
    getDialogueCount() {
        const dialogues = new Set();
        for (const event of this.events) {
            if (event.session && event.dialogue) {
                dialogues.add(`${event.session}::${event.dialogue}`);
            }
        }
        return dialogues.size;
    }

    /**
     * Filter events by session ID
     * @param {string} sessionId
     * @returns {object[]}
     */
    filterBySession(sessionId) {
        return this.events.filter(e => e.session === sessionId);
    }

    /**
     * Filter events by session and dialogue ID
     * @param {string} sessionId
     * @param {string} dialogueId
     * @returns {object[]}
     */
    filterByDialogue(sessionId, dialogueId) {
        return this.events.filter(e =>
            e.session === sessionId && e.dialogue === dialogueId
        );
    }

    /**
     * Get all events
     * @returns {object[]}
     */
    getAllEvents() {
        return this.events;
    }

    /**
     * Calculate statistics for a set of events
     * @param {object[]} events
     * @returns {object}
     */
    calculateStats(events) {
        const stats = {
            totalEvents: events.length,
            byType: {},
            byEvent: {},
            duration: {
                min: null,
                max: null,
                total: 0
            }
        };

        let minTime = Infinity;
        let maxTime = 0;

        for (const event of events) {
            // Count by type
            if (!stats.byType[event.type]) {
                stats.byType[event.type] = 0;
            }
            stats.byType[event.type]++;

            // Count by event
            if (!stats.byEvent[event.event]) {
                stats.byEvent[event.event] = 0;
            }
            stats.byEvent[event.event]++;

            // Track duration
            if (event.duration_us) {
                stats.duration.total += event.duration_us;
                if (stats.duration.min === null || event.duration_us < stats.duration.min) {
                    stats.duration.min = event.duration_us;
                }
                if (stats.duration.max === null || event.duration_us > stats.duration.max) {
                    stats.duration.max = event.duration_us;
                }
            }
        }

        stats.duration.totalMs = Math.round(stats.duration.total / 1000);
        stats.duration.minMs = stats.duration.min ? Math.round(stats.duration.min / 1000) : null;
        stats.duration.maxMs = stats.duration.max ? Math.round(stats.duration.max / 1000) : null;

        return stats;
    }

    /**
     * Group events by flow transitions (from -> to)
     * @param {object[]} events
     * @returns {object}
     */
    groupByTransition(events) {
        const transitions = {};
        for (const event of events) {
            const key = `${event.from} -> ${event.to}`;
            if (!transitions[key]) {
                transitions[key] = [];
            }
            transitions[key].push(event);
        }
        return transitions;
    }

    /**
     * Build timeline of events with relative times
     * @param {object[]} events
     * @returns {object[]}
     */
    buildTimeline(events) {
        if (events.length === 0) return [];

        // Sort by timestamp (approximate from time string)
        const sorted = [...events].sort((a, b) => {
            return a.time.localeCompare(b.time);
        });

        // Parse first event time as baseline
        const baseline = this.parseTimestamp(sorted[0].time);

        return sorted.map(event => ({
            ...event,
            relativeMs: this.parseTimestamp(event.time) - baseline
        }));
    }

    /**
     * Parse timestamp string to milliseconds
     * @param {string} timeStr - Format: "2026-03-13 18:23:55.660"
     * @returns {number} milliseconds since epoch
     */
    parseTimestamp(timeStr) {
        // Parse format: "2026-03-13 18:23:55.660"
        const match = timeStr.match(/(\d{4}-\d{2}-\d{2}) (\d{2}):(\d{2}):(\d{2})\.(\d{3})/);
        if (!match) return 0;

        const [, datePart, hours, minutes, seconds, ms] = match;
        const date = new Date(`${datePart}T${hours}:${minutes}:${seconds}.${ms}`);
        return date.getTime();
    }
}

// Export for use
if (typeof module !== 'undefined' && module.exports) {
    module.exports = FlowTraceParser;
}