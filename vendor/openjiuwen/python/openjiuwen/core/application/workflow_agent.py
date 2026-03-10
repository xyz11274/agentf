class WorkflowAgent:
    def __init__(self, config):
        self.config = config
        self._workflows = []

    def add_workflows(self, workflows):
        self._workflows.extend(workflows)

    @property
    def workflows(self):
        return list(self._workflows)
