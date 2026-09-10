//! Deterministic callback demonstration; no paid model or external tool calls.
use serde_json::{json, Value};
use std::{env, time::Duration};
use trajectory_evolution_client::{Action, Evolution, Run, StepHandle, ModelHandle, Error};

async fn on_model_callback(client: &Evolution, run_id: &str, saved_step: Value, saved_model: Value) -> Result<(), Error> {
    let handle: StepHandle = serde_json::from_value(saved_step).map_err(|_| Error::new("saved_step_invalid"))?;
    let model: ModelHandle = serde_json::from_value(saved_model).map_err(|_| Error::new("saved_model_invalid"))?;
    let step = client.resume(run_id).resume_step(handle);
    step.observe_model(&model, model.request.clone(), json!({"answer": "先处理独立分支"}), "completed", true).await;
    let actions = step.actions(vec![
        Action { id: "slow".into(), operation: "lookup".into(), arguments: json!({"topic": "资料"}) },
        Action { id: "video".into(), operation: "video".into(), arguments: json!({}) },
        Action { id: "fast-failure".into(), operation: "lookup".into(), arguments: json!({"topic": "失败测试"}) },
    ]).await?;
    let mut jobs = Vec::new();
    for action in actions {
        jobs.push(tokio::spawn(action.execute(|spec| async move {
            if spec.id == "fast-failure" { return Err("simulated network failure"); }
            tokio::time::sleep(Duration::from_millis(if spec.id == "slow" { 15 } else { 1 })).await;
            Ok(json!({"found": spec.id}))
        })));
    }
    let mut results = Vec::new();
    // Join all tasks even when one tool returns a business failure.
    for job in jobs { results.push(job.await.map_err(|_| Error::new("callback_interrupted"))?); }
    assert!(results.iter().any(|r| r.id == "slow" && r.status == "completed"));
    assert!(results.iter().any(|r| r.id == "fast-failure" && r.status == "failed"));
    step.end(json!({"results": results}), "completed").await?;
    Ok(())
}

async fn next_callback(client: &Evolution, run: &Run) -> Result<bool, Error> {
    let step = run.step(json!({"image": "missing"}), "main", "assistant").await?;
    if step.handle.decision == "stop" { step.end(json!({"stopped": true}), "completed").await?; return Ok(false); }
    let prepared = step.prepare_model(json!({"system": "你是任务助手。", "messages": [{"role": "user", "content": "视频生成缺少素材"}]})).await?;
    // In a queue consumer these values arrive in the job payload instead.
    tokio::task::yield_now().await;
    on_model_callback(client, &run.id, serde_json::to_value(step.handle).unwrap(), serde_json::to_value(prepared).unwrap()).await?;
    Ok(true)
}

#[tokio::main]
async fn main() -> Result<(), Error> {
    let token = env::var("EVOLUTION_USER_A_TOKEN").map_err(|_| Error::new("token_missing"))?;
    let client = Evolution::new(&env::var("EVOLUTION_URL").unwrap_or("http://127.0.0.1:8766".into()), &token, Duration::from_secs(7))?;
    let run = client.start(&format!("example-rust-{}", uuid::Uuid::new_v4()), json!("视频生成缺少图片素材"), "L3", &["model", "actions", "loop"]).await?;
    // The scheduler drives independent callbacks; the client does not own it.
    for index in 0..15 {
        match next_callback(&client, &run).await {
            Ok(false) => break,
            Ok(true) if index < 14 => {},
            result => {
                let _ = run.abort(json!({"reason": "example_aborted"}), "cancelled").await;
                result?; return Err(Error::new("example_requires_bounded_loop"));
            }
        }
    }
    run.finish(json!({"answer": "检索完成，视频缺少素材"}), "completed", "partial").await?;
    println!("{}", run.status().await?);
    Ok(())
}
