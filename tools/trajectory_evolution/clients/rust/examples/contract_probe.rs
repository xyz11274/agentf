use serde_json::{json, Value};
use std::io::{self, BufRead, Write};
use std::time::Duration;
use trajectory_evolution_client::Evolution;

#[tokio::main]
async fn main() {
    let client = Evolution::with_limits(&std::env::var("EVOLUTION_URL").unwrap(),
        &std::env::var("EVOLUTION_TEST_TOKEN").unwrap(), Duration::from_secs(7), 0, 2097152).unwrap();
    for line in io::stdin().lock().lines() {
        let mut packet: Value = serde_json::from_str(&line.unwrap()).unwrap();
        let fields = packet.as_object_mut().unwrap();
        let operation = fields.remove("operation").unwrap().as_str().unwrap().to_owned();
        let id = fields.remove("request_id").unwrap().as_str().unwrap().to_owned();
        let reply = match client.invoke_id(&operation, packet, &id).await {
            Ok(result) => json!({"ok": true, "result": result}),
            Err(error) => json!({"ok": false, "error": error.code, "status": error.status}),
        };
        println!("{}", reply); io::stdout().flush().unwrap();
    }
}
