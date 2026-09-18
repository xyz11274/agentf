use serde_json::{json, Value};
use trajectory_evolution_client::{CallError, LocalEvolution};

fn require(value: bool, message: &str) {
    if !value {
        panic!("{message}");
    }
}

fn main() {
    let arguments: Vec<String> = std::env::args().collect();
    require(
        arguments.len() == 3,
        "usage: local_contract_probe LIBRARY CONFIG",
    );
    let local = LocalEvolution::new(
        &arguments[1],
        &arguments[2],
        "fixture-principal-alpha-00000001",
        json!({"background": false}),
    )
    .expect("local initialization failed");

    let contract = local.invoke("contract.describe", json!({}));
    require(
        contract["transport"] == "local.native.v1",
        "native contract missing",
    );
    let reference = json!({"run": "rust-native-contract"});
    let begun = local.point(json!({
        "phase": "before",
        "kind": "run",
        "ref": reference,
        "input": "native contract",
        "level": "L0",
        "capabilities": []
    }));
    require(begun["degraded"] != true, "native run degraded");

    let mut calls = 0usize;
    let observed = local
        .call(
            json!({
                "kind": "action",
                "ref": {"run": "rust-native-contract", "key": "action-ok"},
                "name": "lookup",
                "input": {"query": "data"}
            }),
            |input| {
                calls += 1;
                Ok::<Value, &'static str>(input)
            },
        )
        .expect("native action call failed");
    require(
        calls == 1 && observed.executed,
        "business call was not executed once",
    );
    require(observed.after["degraded"] != true, "after receipt degraded");

    let business_error = local
        .call(
            json!({
                "kind": "action",
                "ref": {"run": "rust-native-contract", "key": "action-error"},
                "name": "failing-lookup",
                "input": {"query": "failure"}
            }),
            |_| Err::<Value, _>("original-business-error"),
        )
        .expect_err("business error was swallowed");
    match business_error {
        CallError::Host { source, abort, .. } => {
            require(
                source == "original-business-error",
                "business error was replaced",
            );
            require(abort["degraded"] != true, "abort receipt degraded");
        }
        CallError::Integration(_) => panic!("business error became integration error"),
    }

    let loop_result = local
        .check_loop(json!({
            "ref": {"run": "rust-native-contract", "key": "loop-check"},
            "state": {"round": 0}
        }))
        .expect("loop check failed");
    require(loop_result.allow, "L0 loop unexpectedly stopped");
    require(
        local
            .allow_loop(json!({
                "ref": {"run": "rust-native-contract", "key": "loop-allow"},
                "state": {"round": 1}
            }))
            .expect("allow_loop failed"),
        "allow_loop changed the decision",
    );

    let gap = local
        .capture_gap(json!({"run_id": "not-a-local-run"}), "fixture_gap")
        .expect("capture gap failed");
    require(
        gap["degraded"] == true,
        "invalid gap was reported as complete",
    );
    let management = local
        .manage("agent_evaluation.status", json!({}))
        .expect_err("fixture unexpectedly configured agent evaluation");
    require(
        management.code == "agent_evaluation_not_configured",
        "management ABI error was not preserved",
    );

    let closed = local.point(json!({
        "phase": "after",
        "kind": "run",
        "ref": {"run": "rust-native-contract"},
        "output": "done"
    }));
    require(closed["closed"] == true, "native run did not close");
    local.drain();
    let metrics = local.metrics();
    require(
        metrics["transport"] == "local.native.v1",
        "native metrics missing",
    );
    require(
        metrics["degraded_calls"].is_number(),
        "native counters missing",
    );
    local.set_enabled(false);
    let mut degraded_calls = 0usize;
    let degraded = local
        .call(
            json!({
                "kind": "action",
                "ref": {"run": "rust-native-degraded", "key": "action"},
                "name": "lookup",
                "input": {"query": "still execute"}
            }),
            |input| {
                degraded_calls += 1;
                Ok::<Value, &'static str>(input)
            },
        )
        .expect("degraded observation replaced the business result");
    require(
        degraded_calls == 1 && degraded.executed,
        "degraded observation changed business execution count",
    );
    require(
        degraded.before["degraded"] == true && degraded.after["degraded"] == true,
        "observer failure was not explicit",
    );
    local.set_enabled(true);
    println!(
        "{}",
        json!({"ok": true, "transport": contract["transport"], "calls": calls})
    );
}
