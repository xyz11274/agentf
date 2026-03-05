# Nested MMWorkflow MMAgent Example

This example demonstrates **workflow-to-workflow calls** using the `workflow_ref` feature in AgentFlow.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        Main MMWorkflow                             │
│  ┌─────┐     ┌──────────────┐     ┌──────────────┐     ┌─────┐ │
│  │start│ ──→ │ data_pipeline│ ──→ │final_process │ ──→ │ end │ │
│  └─────┘     │  (workflow)  │     │  (workflow)  │     └─────┘ │
│              └──────┬───────┘     └──────┬───────┘             │
│                     │                    │                     │
└─────────────────────│────────────────────│─────────────────────┘
                      │                    │
                      ▼                    ▼
        ┌─────────────────────┐    ┌─────────────────────┐
        │  Data Pipeline      │    │  Processing MMWorkflow │
        │  ┌───────────────┐  │    │  ┌───────────────┐   │
        │  │  fetch_data   │  │    │  │ aggregate_data│   │
        │  └───────┬───────┘  │    │  └───────┬───────┘   │
        │          │          │    │          │           │
        │          ▼          │    │          ▼           │
        │  ┌───────────────┐  │    │  ┌───────────────┐   │
        │  │ validate_data │  │    │  │generate_output│   │
        │  │  (workflow)   │  │    │  └───────┬───────┘   │
        │  └───────┬───────┘  │    │          │           │
        │          │          │    │          ▼           │
        │          ▼          │    │  ┌───────────────┐   │
        │  ┌───────────────┐  │    │  │persist_results│   │
        │  │transform_data │  │    │  └───────────────┘   │
        │  └───────────────┘  │    │                      │
        └─────────────────────┘    └─────────────────────┘
                      │
                      ▼
        ┌─────────────────────┐
        │ Validation MMWorkflow │
        │  ┌───────────────┐  │
        │  │ check_schema  │  │
        │  └───────┬───────┘  │
        │          │          │
        │          ▼          │
        │  ┌───────────────┐  │
        │  │validate_fields│  │
        │  └───────┬───────┘  │
        │          │          │
        │          ▼          │
        │  ┌───────────────┐  │
        │  │validation_rpt │  │
        │  └───────────────┘  │
        └─────────────────────┘
```

## Files Structure

```
nested_workflow_agent/
├── package.json              # MMAgent package configuration
├── main_workflow.json        # Main orchestrator workflow
├── data_pipeline.json        # Sub-workflow: data processing pipeline
├── validation_workflow.json  # Sub-workflow: validation (called by data_pipeline)
└── processing_workflow.json  # Sub-workflow: final processing
```

## MMWorkflow Nesting Pattern

### Level 1: Main MMWorkflow
- `start` → `data_pipeline` (workflow_ref) → `final_processing` (workflow_ref) → `end`

### Level 2: Data Pipeline
- `fetch_data` → `validate_data` (workflow_ref) → `transform_data`

### Level 3: Validation MMWorkflow (deepest)
- `check_schema` → `validate_fields` → `validation_report`

## Key Features Demonstrated

1. **workflow_ref**: MMNode type `workflow` with `workflow_ref` field for nested workflow calls
2. **Multi-level nesting**: 3 levels deep (main → pipeline → validation)
3. **Sequential orchestration**: Workflows execute in defined order via edges
4. **Shell script execution**: Leaf nodes use `shell_script` execution mode

## Usage

```cpp
// Load agent with nested workflows
auto agent = framework::MMAgent::create_from_package("examples/agent/nested_workflow_agent");

// Initialize and start
agent->initialize();
agent->start();

// Get main workflow
auto main_workflow = agent->get_workflow("main-workflow");

// Verify nested workflow nodes
for (const auto& node : main_workflow->get_nodes()) {
    if (node->get_type() == "workflow") {
        std::cout << "Nested workflow ref: " << node->get_config().workflow_ref << std::endl;
    }
}
```

## Expected Output

When executed, the workflows produce output in this order:

```
Fetching data from source...
Checking data schema...
Validating field values...
Validation passed: all checks completed
Transforming data...
Aggregating processed data...
Generating final output...
Results persisted successfully
```