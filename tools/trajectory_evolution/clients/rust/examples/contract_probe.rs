// rust 客户端：通过统一协议收集执行事件并接收策略建议。
// 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。
// 本文件包含协议验证或接入示例，不作为真实业务效果的评测成绩。

use serde_json::{json, Value};
use std::io::{self, BufRead, Write};
use std::time::Duration;
use trajectory_evolution_client::Evolution;

#[tokio::main]
// 程序入口：读取参数后路由到对应实现，统一处理退出状态和资源收尾。
async fn main() {
    let client = Evolution::with_limits(
        &std::env::var("EVOLUTION_URL").unwrap(),
        &std::env::var("EVOLUTION_TEST_TOKEN").unwrap(),
        Duration::from_secs(7),
        0,
        2097152,
    )
    .unwrap();
    for line in io::stdin().lock().lines() {
        let mut packet: Value = serde_json::from_str(&line.unwrap()).unwrap();
        let fields = packet.as_object_mut().unwrap();
        let operation = fields
            .remove("operation")
            .unwrap()
            .as_str()
            .unwrap()
            .to_owned();
        let id = fields
            .remove("request_id")
            .unwrap()
            .as_str()
            .unwrap()
            .to_owned();
        let reply = match client.invoke_id(&operation, packet, &id).await {
            Ok(result) => json!({"ok": true, "result": result}),
            Err(error) => json!({"ok": false, "error": error.code, "status": error.status}),
        };
        println!("{}", reply);
        io::stdout().flush().unwrap();
    }
}
