// rust 客户端：通过统一协议收集执行事件并接收策略建议。
// 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。
// 本文件包含协议验证或接入示例，不作为真实业务效果的评测成绩。

//! 确定性的回调示例，不调用付费模型或外部工具。
use serde_json::{json, Value};
use std::{env, time::Duration};
use trajectory_evolution_client::{Action, Error, Evolution, ModelHandle, Run, StepHandle};

// 示范在原模型回调前后插入打点，宿主调度方式保持不变。
async fn on_model_callback(
    client: &Evolution,
    run_id: &str,
    saved_step: Value,
    saved_model: Value,
) -> Result<(), Error> {
    let handle: StepHandle =
        serde_json::from_value(saved_step).map_err(|_| Error::new("saved_step_invalid"))?;
    let model: ModelHandle =
        serde_json::from_value(saved_model).map_err(|_| Error::new("saved_model_invalid"))?;
    let step = client.resume(run_id).resume_step(handle);
    step.observe_model(
        &model,
        model.request.clone(),
        json!({"answer": "先处理独立分支"}),
        "completed",
        true,
    )
    .await;
    let actions = step
        .actions(vec![
            Action {
                id: "slow".into(),
                operation: "lookup".into(),
                arguments: json!({"topic": "资料"}),
            },
            Action {
                id: "video".into(),
                operation: "video".into(),
                arguments: json!({}),
            },
            Action {
                id: "fast-failure".into(),
                operation: "lookup".into(),
                arguments: json!({"topic": "失败测试"}),
            },
        ])
        .await?;
    let mut jobs = Vec::new();
    for action in actions {
        jobs.push(tokio::spawn(action.execute(|spec| async move {
            if spec.id == "fast-failure" {
                return Err("simulated network failure");
            }
            tokio::time::sleep(Duration::from_millis(if spec.id == "slow" {
                15
            } else {
                1
            }))
            .await;
            Ok(json!({"found": spec.id}))
        })));
    }
    let mut results = Vec::new();
    // 即使某个工具业务失败，也等待所有独立任务完成。
    for job in jobs {
        results.push(job.await.map_err(|_| Error::new("callback_interrupted"))?);
    }
    assert!(results
        .iter()
        .any(|r| r.id == "slow" && r.status == "completed"));
    assert!(results
        .iter()
        .any(|r| r.id == "fast-failure" && r.status == "failed"));
    step.end(json!({"results": results}), "completed").await?;
    Ok(())
}

// 把后续任务交回原调度器，传递可序列化上下文而非整个 Agent 对象。
async fn next_callback(client: &Evolution, run: &Run) -> Result<bool, Error> {
    let step = run
        .step(json!({"image": "missing"}), "main", "assistant")
        .await?;
    if step.handle.decision == "stop" {
        step.end(json!({"stopped": true}), "completed").await?;
        return Ok(false);
    }
    let prepared = step.prepare_model(json!({"system": "你是任务助手。", "messages": [{"role": "user", "content": "视频生成缺少素材"}]})).await?;
    // 在队列消费者中，这些值应从任务消息中获得。
    tokio::task::yield_now().await;
    on_model_callback(
        client,
        &run.id,
        serde_json::to_value(step.handle).unwrap(),
        serde_json::to_value(prepared).unwrap(),
    )
    .await?;
    Ok(true)
}

#[tokio::main]
// 程序入口：读取参数后路由到对应实现，统一处理退出状态和资源收尾。
async fn main() -> Result<(), Error> {
    let token = env::var("EVOLUTION_USER_A_TOKEN").map_err(|_| Error::new("token_missing"))?;
    let client = Evolution::new(
        &env::var("EVOLUTION_URL").unwrap_or("http://127.0.0.1:8766".into()),
        &token,
        Duration::from_secs(7),
    )?;
    let run = client
        .start(
            &format!("example-rust-{}", uuid::Uuid::new_v4()),
            json!("视频生成缺少图片素材"),
            "L3",
            &["model", "actions", "loop"],
        )
        .await?;
    // 独立回调由宿主调度器驱动，客户端不接管调度器。
    for index in 0..15 {
        match next_callback(&client, &run).await {
            Ok(false) => break,
            Ok(true) if index < 14 => {}
            result => {
                let _ = run
                    .abort(json!({"reason": "example_aborted"}), "cancelled")
                    .await;
                result?;
                return Err(Error::new("example_requires_bounded_loop"));
            }
        }
    }
    run.finish(
        json!({"answer": "检索完成，视频缺少素材"}),
        "completed",
        "partial",
    )
    .await?;
    println!("{}", run.status().await?);
    Ok(())
}
