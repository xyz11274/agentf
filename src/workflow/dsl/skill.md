# Workflow DSL Skill Specification

## Skill Name
Workflow Orchestration Skill

## Capability
Parse and orchestrate workflow definitions, including nested workflows.

## Inputs
- workflow json/jsonc content
- optional file-based includes

## Invocation
1. Parse workflow metadata/nodes/edges.
2. Resolve `includes` files relative to current workflow file.
3. Merge included workflows into current graph.
4. Support `type: workflow` node with `workflow_ref`.

## Outputs
- parsed workflow graph
- validation errors/warnings

## Control Contracts
- edge control: `if/loop/fork/join/async/goto`
- include contract: `"includes": ["relative/path/subflow.json"]`
