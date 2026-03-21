/**
 * FlowTraceViewer - Main Application
 */
class FlowTraceViewer {
    constructor() {
        this.parser = new FlowTraceParser();
        this.visualizer = new FlowTraceVisualizer();
        this.currentSession = null;
        this.currentDialogue = null;
    }

    /**
     * Initialize the application
     */
    async init() {
        await this.visualizer.init();
        this.setupEventListeners();
        this.setupDragDrop();
    }

    /**
     * Setup event listeners
     */
    setupEventListeners() {
        // File input
        const fileInput = document.getElementById('file-input');
        if (fileInput) {
            fileInput.addEventListener('change', (e) => this.handleFileSelect(e));
        }

        // Session selector
        const sessionSelect = document.getElementById('session-select');
        if (sessionSelect) {
            sessionSelect.addEventListener('change', (e) => this.handleSessionChange(e));
        }

        // Dialogue selector
        const dialogueSelect = document.getElementById('dialogue-select');
        if (dialogueSelect) {
            dialogueSelect.addEventListener('change', (e) => this.handleDialogueChange(e));
        }

        // View tabs
        const tabs = document.querySelectorAll('.tab-btn');
        tabs.forEach(tab => {
            tab.addEventListener('click', (e) => this.handleTabChange(e));
        });

        // Load sample button
        const loadSampleBtn = document.getElementById('load-sample');
        if (loadSampleBtn) {
            loadSampleBtn.addEventListener('click', () => this.loadSampleData());
        }
    }

    /**
     * Setup drag and drop
     */
    setupDragDrop() {
        const dropZone = document.getElementById('drop-zone');
        if (!dropZone) return;

        dropZone.addEventListener('dragover', (e) => {
            e.preventDefault();
            dropZone.classList.add('dragover');
        });

        dropZone.addEventListener('dragleave', () => {
            dropZone.classList.remove('dragover');
        });

        dropZone.addEventListener('drop', (e) => {
            e.preventDefault();
            dropZone.classList.remove('dragover');
            const files = e.dataTransfer.files;
            if (files.length > 0) {
                this.loadFile(files[0]);
            }
        });
    }

    /**
     * Handle file selection
     */
    handleFileSelect(event) {
        const files = event.target.files;
        if (files.length > 0) {
            this.loadFile(files[0]);
        }
    }

    /**
     * Load a file
     */
    async loadFile(file) {
        const reader = new FileReader();
        reader.onload = (e) => {
            this.parseAndDisplay(e.target.result, file.name);
        };
        reader.onerror = () => {
            this.showError('Failed to read file');
        };
        reader.readAsText(file);
    }

    /**
     * Parse and display trace data
     */
    parseAndDisplay(content, filename = 'trace.log') {
        try {
            this.parser.parseFile(content);
            this.updateStats();
            this.updateSelectors();
            this.displayCurrentView();
            this.showSuccess(`已加载 ${this.parser.events.length} 条事件（来自 ${filename}）`);
        } catch (e) {
            this.showError(`文件解析失败: ${e.message}`);
        }
    }

    /**
     * Update statistics display
     */
    updateStats() {
        const stats = this.parser.calculateStats(this.parser.getAllEvents());

        const statsEl = document.getElementById('stats');
        if (statsEl) {
            statsEl.innerHTML = `
                <div class="stat-item">
                    <span class="stat-label">事件总数</span>
                    <span class="stat-value">${stats.totalEvents}</span>
                </div>
                <div class="stat-item">
                    <span class="stat-label">会话数</span>
                    <span class="stat-value">${this.parser.getSessions().length}</span>
                </div>
                <div class="stat-item">
                    <span class="stat-label">对话数</span>
                    <span class="stat-value">${this.parser.getDialogueCount()}</span>
                </div>
                <div class="stat-item">
                    <span class="stat-label">总耗时</span>
                    <span class="stat-value">${stats.duration.totalMs}ms</span>
                </div>
            `;
        }
    }

    /**
     * Update session and dialogue selectors
     */
    updateSelectors() {
        const sessions = this.parser.getSessions();

        // Update session selector
        const sessionSelect = document.getElementById('session-select');
        if (sessionSelect) {
            sessionSelect.innerHTML = '<option value="">All Sessions</option>';
            for (const session of sessions) {
                sessionSelect.innerHTML += `<option value="${session}">${session}</option>`;
            }
        }

        // Reset dialogue selector
        const dialogueSelect = document.getElementById('dialogue-select');
        if (dialogueSelect) {
            dialogueSelect.innerHTML = '<option value="">All Dialogues</option>';
        }

        this.currentSession = null;
        this.currentDialogue = null;
    }

    /**
     * Handle session selection change
     */
    handleSessionChange(event) {
        this.currentSession = event.target.value || null;
        this.currentDialogue = null;

        // Update dialogue selector
        const dialogueSelect = document.getElementById('dialogue-select');
        if (dialogueSelect) {
            dialogueSelect.innerHTML = '<option value="">All Dialogues</option>';
            if (this.currentSession) {
                const dialogues = this.parser.getDialogues(this.currentSession);
                for (const dialogue of dialogues) {
                    dialogueSelect.innerHTML += `<option value="${dialogue}">${dialogue}</option>`;
                }
            }
        }

        this.displayCurrentView();
    }

    /**
     * Handle dialogue selection change
     */
    handleDialogueChange(event) {
        this.currentDialogue = event.target.value || null;
        this.displayCurrentView();
    }

    /**
     * Handle tab change
     */
    handleTabChange(event) {
        const tabBtn = event.target;
        const tabName = tabBtn.dataset.tab;

        // Update active tab
        document.querySelectorAll('.tab-btn').forEach(btn => btn.classList.remove('active'));
        tabBtn.classList.add('active');

        // Update active content
        document.querySelectorAll('.tab-content').forEach(content => content.classList.remove('active'));
        document.getElementById(`tab-${tabName}`).classList.add('active');

        this.displayCurrentView();
    }

    /**
     * Display current view based on selections
     */
    async displayCurrentView() {
        let events;
        if (this.currentSession && this.currentDialogue) {
            events = this.parser.filterByDialogue(this.currentSession, this.currentDialogue);
        } else if (this.currentSession) {
            events = this.parser.filterBySession(this.currentSession);
        } else {
            events = this.parser.getAllEvents();
        }

        // Get active tab
        const activeTab = document.querySelector('.tab-btn.active');
        const tabName = activeTab ? activeTab.dataset.tab : 'sequence';

        switch (tabName) {
            case 'sequence':
                await this.displaySequenceDiagram(events);
                break;
            case 'flow':
                await this.displayFlowDiagram(events);
                break;
            case 'table':
                this.displayTimingTable(events);
                break;
        }

        // Update filtered stats
        this.displayFilteredStats(events);
    }

    /**
     * Display sequence diagram
     */
    async displaySequenceDiagram(events) {
        const code = this.visualizer.generateSequenceDiagram(events);
        await this.visualizer.render('sequence-diagram', code);

        // Show code
        const codeEl = document.getElementById('sequence-code');
        if (codeEl) {
            codeEl.textContent = code;
        }
    }

    /**
     * Display flow diagram
     */
    async displayFlowDiagram(events) {
        const code = this.visualizer.generateFlowDiagram(events);
        await this.visualizer.render('flow-diagram', code);

        // Show code
        const codeEl = document.getElementById('flow-code');
        if (codeEl) {
            codeEl.textContent = code;
        }
    }

    /**
     * Display timing table
     */
    displayTimingTable(events) {
        const tableEl = document.getElementById('timing-table');
        if (tableEl) {
            tableEl.innerHTML = this.visualizer.generateTimingTable(events);
        }
    }

    /**
     * Display filtered statistics
     */
    displayFilteredStats(events) {
        const stats = this.parser.calculateStats(events);
        const statsEl = document.getElementById('filtered-stats');
        if (statsEl) {
            statsEl.innerHTML = `
                <div class="stat-item">
                    <span class="stat-label">显示事件</span>
                    <span class="stat-value">${events.length} 条</span>
                </div>
                <div class="stat-item">
                    <span class="stat-label">耗时合计</span>
                    <span class="stat-value">${stats.duration.totalMs}ms</span>
                </div>
            `;
        }
    }

    /**
     * Load sample data for demo
     */
    loadSampleData() {
        const sampleData = `{"time":"2026-03-13 18:23:55.658","type":"task-exec","session":"session-001","dialogue":"12345678","from":"external","to":"dispatcher","event":"dialogue-created"}
{"time":"2026-03-13 18:23:55.659","type":"external-call","session":"session-001","dialogue":"12345678","from":"agent-1","to":"ctx-callback","event":"enter-callback","inputs":"user_id=u001|user_input=帮我订一张去北京的机票","outputs":"上下文：用户偏好商务舱"}
{"time":"2026-03-13 18:23:55.660","type":"task-exec","session":"session-001","dialogue":"12345678","from":"external","to":"dispatcher","event":"task-created","inputs":"{\\"user_text\\":\\"帮我订一张去北京的机票\\"}"}
{"time":"2026-03-13 18:23:55.661","type":"task-exec","session":"session-001","dialogue":"12345678","from":"dispatcher","to":"skill-selection","event":"task-dispatched"}
{"time":"2026-03-13 18:23:55.662","type":"workflow-exec","session":"session-001","dialogue":"12345678","from":"task","to":"skill-selection-workflow","event":"workflow-started"}
{"time":"2026-03-13 18:23:55.663","type":"node-exec","session":"session-001","dialogue":"12345678","from":"skill-selection-workflow","to":"select_skills","event":"node-started"}
{"time":"2026-03-13 18:23:55.670","type":"node-exec","session":"session-001","dialogue":"12345678","from":"skill-selection-workflow","to":"select_skills","event":"node-completed","duration_us":7000,"outputs":"{\\"next_skills\\":[\\"travel\\",\\"shopping\\"]}"}
{"time":"2026-03-13 18:23:55.671","type":"workflow-exec","session":"session-001","dialogue":"12345678","from":"task","to":"skill-selection-workflow","event":"workflow-completed","duration_us":9000}
{"time":"2026-03-13 18:23:55.672","type":"task-exec","session":"session-001","dialogue":"12345678","from":"skill-selection","to":"dispatcher","event":"data-exchange"}
{"time":"2026-03-13 18:23:55.673","type":"skill-exec","session":"session-001","dialogue":"12345678","from":"dispatcher","to":"travel","event":"skill-launched"}
{"time":"2026-03-13 18:23:55.674","type":"workflow-exec","session":"session-001","dialogue":"12345678","from":"task","to":"travel-workflow","event":"workflow-started"}
{"time":"2026-03-13 18:23:55.675","type":"node-exec","session":"session-001","dialogue":"12345678","from":"travel-workflow","to":"travel_node","event":"node-started"}
{"time":"2026-03-13 18:23:55.680","type":"node-exec","session":"session-001","dialogue":"12345678","from":"travel-workflow","to":"travel_node","event":"node-completed","duration_us":5000,"outputs":"{\\"travel_result\\":\\"查询到3个航班...\\"}"}
{"time":"2026-03-13 18:23:55.681","type":"workflow-exec","session":"session-001","dialogue":"12345678","from":"task","to":"travel-workflow","event":"workflow-completed","duration_us":7000}
{"time":"2026-03-13 18:23:55.700","type":"task-exec","session":"session-001","dialogue":"12345678","from":"coordinator","to":"external","event":"response-sent","outputs":"{\\"response\\":\\"已为您查询到航班信息...\\"}"}
{"time":"2026-03-13 18:23:55.701","type":"task-exec","session":"session-001","dialogue":"12345678","from":"external","to":"dispatcher","event":"dialogue-completed","duration_us":41000}`;

        this.parseAndDisplay(sampleData, 'sample_data.json');
    }

    /**
     * Show success message
     */
    showSuccess(message) {
        const statusEl = document.getElementById('status');
        if (statusEl) {
            statusEl.className = 'status success';
            statusEl.textContent = message;
            setTimeout(() => statusEl.className = 'status', 3000);
        }
    }

    /**
     * Show error message
     */
    showError(message) {
        const statusEl = document.getElementById('status');
        if (statusEl) {
            statusEl.className = 'status error';
            statusEl.textContent = message;
        }
    }
}

// Initialize on page load
document.addEventListener('DOMContentLoaded', async () => {
    const app = new FlowTraceViewer();
    await app.init();
    window.app = app; // For debugging
});