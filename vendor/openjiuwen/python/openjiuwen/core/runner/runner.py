class _ResourceManager:
    def __init__(self):
        self._workflows = {}

    def add_workflow(self, card, factory):
        self._workflows[getattr(card, 'id', str(card))] = factory


class _Runner:
    def __init__(self):
        self.resource_mgr = _ResourceManager()

    async def run_agent(self, workflow_agent, inputs):
        workflows = getattr(workflow_agent, 'workflows', [])
        if not workflows:
            return {'output': type('Result', (), {'result': {'response': 'no workflow registered'}})()}
        workflow = workflows[0]
        return await workflow.run(inputs)


Runner = _Runner()
