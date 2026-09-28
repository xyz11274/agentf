// local 回归验证：检查包内协议、边界条件与失败处理。
// 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
// 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

import assert from 'node:assert/strict';
import {LocalEvolution} from '../../clients/javascript/evolution-local.mjs';

const c = new LocalEvolution({
  library : `${process.argv[2]}/trajectory_evolution_local_node.node`,
  config : process.argv[3],
  token : process.env.INTEGRATION_TEST_A
});
const call = (op, fields = {}) => {
  const result = c.invoke(op, fields);
  assert.equal(result.degraded, false, JSON.stringify(result));
  return result;
};
try {
  const {run_id} =
      call('run.begin', {input : '视频生成缺少素材', level : 'L1', capabilities : [ 'model' ]});
  const model = call('activity.begin', {
    run_id,
    kind : 'model',
    name : 'reason',
    input : {system : '原始指令'},
    binding : {pointer : '/system', mode : 'text'}
  });
  assert.match(model.input.system, /先补充图片/);
  call('activity.end', {
    run_id,
    activity_id : model.activity_id,
    actual_input : model.input,
    executed : true,
    status : 'completed',
    output : '完成'
  });
  call('run.end', {run_id, output : '完成', status : 'completed'});
  for (let i = 0; i < 200; ++i) {
    if (call('run.status', {run_id}).evaluation_status === 'completed')
      break;
    await new Promise(resolve => setTimeout(resolve, 20));
  }
  assert.equal(c.metrics().published_runs, 1);
  assert.equal(call('run.status', {run_id}).evaluation_status, 'completed');
  c.setEnabled(false);
  assert.equal(c.invoke('run.begin', {input : '暂停'}).degraded, true);
  c.setEnabled(true);
  assert.equal(c.invoke('event.record', {run_id, data : NaN}).degraded, true);
  assert.equal(c.invoke('activity.begin', null).degraded, true);
} finally {
  c.close();
}
