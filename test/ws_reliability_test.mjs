// 可靠投递线的端到端回归（B1 逐条 ACK / B3 线格式带序号 / B4 重连补齐）
//
// 用法：
//   node test/ws_reliability_test.mjs [ws://127.0.0.1:19096/chat]
//
// 前置：
//   · 单实例，enable_cluster_bus = false（与 ws_e2e_test.mjs 同一前提）
//   · enable_message_dedup 关（本脚本不复用 key）
//   · 默认 LOONG_WS_JOURNAL_CAP（512）即可
//
// 覆盖：
//   B1  ACK：成功 200 / 背压 503 / 私聊离线 404，且 requestId 原样回传
//   B1  不发 requestId 的老客户端【收不到】ACK（向后兼容）
//   B3  房间广播的线格式带单调递增的 seq
//   B4  sync：断线期间的缺口按 seq 精确补齐，sync_done 报出房间当前最大序号
//   B4  sync：游标超出可回放范围时明确回 206（不假装补齐成功）
//   B4  sync：sinceSeq = 0（没有游标）不回放任何历史

const BASE = process.argv[2] || 'ws://127.0.0.1:19096/chat';
const ROOM = `rel_${Date.now()}`;

let failures = 0;
const ok = (cond, msg) => {
  console.log(`  [${cond ? ' ok ' : 'FAIL'}] ${msg}`);
  if (!cond) failures++;
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function connect(name, room = ROOM) {
  return new Promise((resolve, reject) => {
    const url = `${BASE}?room_name=${encodeURIComponent(room)}&name=${encodeURIComponent(name)}`;
    const ws = new WebSocket(url);
    const inbox = [];
    const waiters = [];
    const timer = setTimeout(() => reject(new Error(`连接超时: ${name}`)), 5000);

    ws.addEventListener('message', (ev) => {
      let data;
      try {
        data = JSON.parse(ev.data);
      } catch {
        return;
      }
      // ⚠️ 不能 waiters.shift()：那会让「第一条不匹配的消息」把等待器吃掉，
      // 后面的目标消息就永远唤不醒它了（本脚本第一版正是这么挂的）。
      for (let i = 0; i < waiters.length; i++) {
        if (waiters[i].pred(data)) {
          const w = waiters.splice(i, 1)[0];
          w.resolve(data);
          return;
        }
      }
      inbox.push(data);
    });
    ws.addEventListener('open', () => {
      clearTimeout(timer);
      resolve({
        name,
        ws,
        inbox,
        send: (obj) => ws.send(JSON.stringify(obj)),
        drain: () => inbox.splice(0, inbox.length),
        // 按谓词等一条消息（不匹配的消息留在 inbox 里，不丢；等待器也不会被误吃）
        wait: (pred, timeoutMs = 3000) =>
          new Promise((res, rej) => {
            const idx = inbox.findIndex(pred);
            if (idx >= 0) return res(inbox.splice(idx, 1)[0]);
            const t = setTimeout(() => {
              const i = waiters.findIndex((w) => w.pred === pred);
              if (i >= 0) waiters.splice(i, 1);
              rej(new Error(`等待消息超时: ${name}`));
            }, timeoutMs);
            waiters.push({
              pred,
              resolve: (d) => {
                clearTimeout(t);
                res(d);
              },
            });
          }),
        close: () => ws.close(),
      });
    });
    ws.addEventListener('error', () => reject(new Error(`连接失败: ${name}`)));
  });
}

const isAck = (rid) => (m) => m.type === 'ack' && m.requestId === rid;
const isMsg = (m) => m.type === 'message';
const isSyncDone = (m) => m.type === 'sync_done';

// ── B1：逐条 ACK ────────────────────────────────────────────────────────────
console.log('B1 · 逐条 ACK');
{
  const a = await connect('ack_alice');
  await sleep(600);
  a.drain();

  // ① 成功路径
  a.send({ key: 'k1', requestId: 'r1', action: 'message', msgContent: 'hello', toUser: '' });
  const ack1 = await a.wait(isAck('r1'));
  ok(ack1.code === 200, `成功投递 → ACK code=200（实际 ${ack1.code}）`);
  ok(ack1.id > 0, `ACK 带回消息 id（实际 ${ack1.id}）`);
  ok(ack1.seq > 0, `ACK 带回房间序号（实际 ${ack1.seq}）`);

  // ② 私聊目标离线 → 404（终态，客户端应停止重试）
  a.send({
    key: 'k2',
    requestId: 'r2',
    action: 'message',
    msgContent: 'to-ghost',
    toUser: 'ghost_user_nobody',
  });
  const ack2 = await a.wait(isAck('r2'));
  ok(ack2.code === 404, `私聊离线目标 → ACK code=404（实际 ${ack2.code}）`);
  ok(typeof ack2.message === 'string' && ack2.message.length > 0, 'ACK 带可读失败原因');

  // ③ 空内容 → 400
  a.send({ key: 'k3', requestId: 'r3', action: 'message', msgContent: '', toUser: '' });
  const ack3 = await a.wait(isAck('r3'));
  ok(ack3.code === 400, `空内容 → ACK code=400（实际 ${ack3.code}）`);

  // ④ 老客户端（不发 requestId）不应收到任何 ACK
  const before = a.inbox.length;
  a.send({ key: 'k4', action: 'message', msgContent: 'legacy', toUser: '' });
  await sleep(800);
  const acks = a.inbox.filter((m) => m.type === 'ack');
  ok(acks.length === 0, `不发 requestId 则收不到 ACK（inbox 从 ${before} 变 ${a.inbox.length}，ACK 数 ${acks.length}）`);

  a.close();
}

// ── B3：线格式带单调递增的房间序号 ──────────────────────────────────────────
console.log('B3 · 线格式的房间序号');
let bobLastSeq = 0;
{
  const a = await connect('seq_alice');
  const b = await connect('seq_bob');
  await sleep(600);
  a.drain();
  b.drain();

  for (let i = 0; i < 5; i++) {
    a.send({ key: `s${i}`, requestId: `sr${i}`, action: 'message', msgContent: `seq_${i}`, toUser: '' });
  }
  await sleep(1200);

  const msgs = b.inbox.filter(isMsg).filter((m) => m.message.startsWith('seq_'));
  ok(msgs.length === 5, `观察者收到 5 条（实际 ${msgs.length}）`);
  const seqs = msgs.map((m) => m.seq);
  ok(seqs.every((s) => s > 0), `每条都带非零 seq（${seqs.join(',')}）`);
  ok(
    seqs.every((s, i) => i === 0 || s > seqs[i - 1]),
    `seq 严格递增（${seqs.join(',')}）`
  );
  bobLastSeq = seqs[seqs.length - 1];

  a.close();
  b.close();
}

// ── B4：断线期间的缺口按 seq 补齐 ───────────────────────────────────────────
console.log('B4 · 断线补齐');
{
  const a = await connect('sync_alice');
  const b1 = await connect('sync_bob');
  await sleep(600);
  a.drain();
  b1.drain();

  a.send({ key: 'p1', requestId: 'pr1', action: 'message', msgContent: 'before_cut', toUser: '' });
  await sleep(800);
  const before = b1.inbox.filter(isMsg).find((m) => m.message === 'before_cut');
  ok(!!before, '断线前 bob 收到了 before_cut');
  const cursor = before ? before.seq : 0;
  b1.close(); // bob 掉线
  await sleep(400);

  // bob 不在的时候，alice 发 3 条
  for (let i = 0; i < 3; i++) {
    a.send({ key: `g${i}`, requestId: `gr${i}`, action: 'message', msgContent: `missed_${i}`, toUser: '' });
  }
  await sleep(1000);

  // bob 重连并带上游标
  const b2 = await connect('sync_bob');
  await sleep(300);
  b2.drain(); // 丢掉入群公告等
  b2.send({ action: 'sync', sinceSeq: cursor });

  const done = await b2.wait(isSyncDone, 4000);
  const replayed = b2.inbox.filter(isMsg).map((m) => m.message).filter((t) => t.startsWith('missed_'));
  ok(replayed.length === 3, `缺口 3 条全部补齐（实际 ${replayed.length}：${JSON.stringify(replayed)}）`);
  ok(
    replayed.join(',') === 'missed_0,missed_1,missed_2',
    `补齐顺序与发送顺序一致（实际 ${replayed.join(',')}）`
  );
  ok(done.code === 200, `游标在可回放范围内 → sync_done code=200（实际 ${done.code}）`);
  ok(done.seq >= cursor + 3, `sync_done 报出房间当前最大序号（${cursor} + 3 ≤ ${done.seq}）`);

  // 游标超范围 → 明确回 206，不假装补齐
  b2.drain();
  b2.send({ action: 'sync', sinceSeq: 999999999 });
  const done2 = await b2.wait(isSyncDone, 4000);
  ok(done2.code === 206, `游标超出可回放范围 → sync_done code=206（实际 ${done2.code}）`);

  // 没有游标（0）→ 不回放任何历史
  b2.drain();
  b2.send({ action: 'sync', sinceSeq: 0 });
  const done3 = await b2.wait(isSyncDone, 4000);
  const hist = b2.inbox.filter(isMsg);
  ok(hist.length === 0 && done3.code === 200, `sinceSeq=0 → 不回放历史（实际回放 ${hist.length} 条）`);

  // ── #3 游标按实例作用域：换了实例必须给出准确原因 ──
  //
  // 房间序号是【每实例】的：带着 A 实例的游标连到 B 实例，这两个数没有关系。
  // 服务端必须 ① 不回放（免得拿错序号空间乱补）② 如实报 206
  // ③ 说清是「另一实例」而不是含糊的「超出可回放范围」—— 后者会把人引向
  // 「日志容量太小」这个错误方向（实际根因是换了台机器）。
  const myInstance = done.instance;
  ok(
    typeof myInstance === 'string' && myInstance.length > 0,
    `sync_done 告知当前实例标识（${myInstance}）`
  );

  b2.drain();
  b2.send({ action: 'sync', sinceSeq: 12345, sinceInstance: 'inst_from_another_box' });
  const done4 = await b2.wait(isSyncDone, 4000);
  ok(done4.code === 206, `另一实例的游标 → code=206（实际 ${done4.code}）`);
  ok(/另一实例/.test(done4.message || ''), `原因写明「另一实例」（实际「${done4.message}」）`);
  ok(b2.inbox.filter(isMsg).length === 0, '不回放（避免拿错序号空间乱补）');

  // 同一个实例、游标确实超范围 → 仍是 206，但原因不该说「另一实例」
  b2.drain();
  b2.send({ action: 'sync', sinceSeq: 999999999, sinceInstance: myInstance });
  const done5 = await b2.wait(isSyncDone, 4000);
  ok(done5.code === 206, `本实例的越界游标 → code=206（实际 ${done5.code}）`);
  ok(
    !/另一实例/.test(done5.message || ''),
    `且不会被误报成「另一实例」（实际「${done5.message}」）`
  );

  a.close();
  b2.close();
}

console.log(failures === 0 ? '\nALL PASSED (failures=0)' : `\n${failures} 项失败`);
process.exit(failures === 0 ? 0 : 1);
