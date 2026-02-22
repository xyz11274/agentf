# Agent Package Specification

## 1. Package naming
- Outer package: `$xxxAgent_$PKGTYPE_$PKGVER.tar.gz`
- Inner object package: `$xxxAgent_$AGTVER.tar.gz`

Where:
- `$xxxAgent`: Agent package name (must end with `Agent`)
- `$PKGTYPE`: package type, for example `release` / `snapshot`
- `$PKGVER`: package version
- `$AGTVER`: Agent object version

## 2. Package structure

```text
$xxxAgent_$PKGTYPE_$PKGVER.tar.gz
├── $xxxAgent_$AGTVER.tar.gz
└── package.json
```

Inner object package content:

```text
$xxxAgent_$AGTVER.tar.gz/
├── agent.json
├── agentCard.md
├── workflow_XX.json
└── node_xxx.$TYPE
```

## 3. Required files

### 3.1 `agent.json`
Runtime agent metadata:
- `name`: Agent name
- `version`: Agent version (`$AGTVER`)
- `description`: summary
- `workflows`: workflow file list

### 3.2 `agentCard.md`
Prompt and capability card for model/agent interoperability.

### 3.3 `workflow_XX.json`
Workflow graph with `node` and `edge` definitions. Nodes should declare executable artifact binding.

### 3.4 `node_xxx.$TYPE`
Executable node implementation; supported extensions include `.so`, `.sh`, `.py`, `.jar`.

### 3.5 `package.json`
Outer package descriptor:
- package name/version/type
- agent version
- package summary
- packaged file manifest

## 4. Validation checklist
- Outer/inner file names must match spec.
- `agent.json.version` must equal inner package version suffix.
- `package.json.version` must equal outer package version suffix.
- `package.json.file_manifest` must include `package.json` and inner package tarball.
