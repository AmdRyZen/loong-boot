// loong-boot 聊天室 P0 修复回归测试（端到端）
// 覆盖：房间广播 FIFO 顺序 / 私聊投递 / 同名多端不串号 / 离线提示
//
// 用法：
//   node test/ws_e2e_test.mjs [ws://127.0.0.1:9091/chat]
//
// 其中「同名多端不串号」是本轮 P0 修复的核心回归项：
//   旧实现用 昵称->单连接 注册表，旧连接断开时会按昵称 erase，
//   把同名新连接的记录一并删掉，导致新用户从此收不到私聊。

const BASE = process.argv[2] || 'ws://127.0.0.1:9091/chat';
const ROOM = `e2e_${Date.now()}`;

let failures = 0;
const ok = (cond, msg) => {
  console.log(`  [${cond ? ' ok ' : 'FAIL'}] ${msg}`);
  if (!cond) failures++;
};

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
      const waiter = waiters.shift();
      if (waiter) waiter(data);
      else inbox.push(data);
    });
    ws.addEventListener('open', () => {
      clearTimeout(timer);
      resolve({
        name,
        ws,
        inbox,
        send: (obj) => ws.send(JSON.stringify(obj)),
        drain: () => inbox.splice(0, inbox.length),
        next: (timeoutMs = 3000) =>
          new Promise((res, rej) => {
            if (inbox.length) return res(inbox.shift());
            const t = setTimeout(() => {
              const i = waiters.indexOf(wrap);
              if (i >= 0) waiters.splice(i, 1);
              rej(new Error(`等待消息超时: ${name}`));
            }, timeoutMs);
            const wrap = (d) => {
              clearTimeout(t);
              res(d);
            };
            waiters.push(wrap);
          }),
      });
    });
    ws.addEventListener('error', () => reject(new Error(`连接失败: ${name}`)));
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// 收集一段时间内的所有消息
async function collect(client, ms) {
  const out = [];
  const deadline = Date.now() + ms;
  while (Date.now() < deadline) {
    try {
      out.push(await client.next(Math.max(1, deadline - Date.now())));
    } catch {
      break;
    }
  }
  return out;
}

async function testRoomOrdering() {
  console.log('test: 房间广播 FIFO 顺序（P0 乱序修复）');
  const a = await connect('alice');
  const b = await connect('bob');
  const c = await connect('carol');
  await sleep(300);
  a.drain();
  b.drain();
  c.drain();

  const N = 300;
  for (let i = 0; i < N; i++) {
    a.send({ key: ROOM, action: 'message', msgContent: `seq_${i}`, toUser: '' });
  }

  const got = await collect(b, 4000);
  const seq = got
    .filter((m) => typeof m.message === 'string' && m.message.startsWith('seq_'))
    .map((m) => Number(m.message.slice(4)));

  ok(seq.length === N, `广播无丢失（期望 ${N} 条，实收 ${seq.length} 条）`);
  let ordered = true;
  for (let i = 0; i < seq.length; i++) {
    if (seq[i] !== i) {
      ordered = false;
      break;
    }
  }
  ok(ordered, '同一房间消息严格按发送顺序到达（无乱序）');

  a.ws.close();
  b.ws.close();
  c.ws.close();
  await sleep(200);
}

async function testDirectMessage() {
  console.log('test: 点对点私聊');
  const a = await connect('alice2');
  const b = await connect('bob2');
  await sleep(300);
  a.drain();
  b.drain();

  a.send({ key: ROOM, action: 'message', msgContent: 'hello-bob', toUser: 'bob2' });

  const recv = await b.next();
  ok(recv.message === '[私聊] hello-bob', `目标收到私聊（实际: ${recv.message}）`);
  const echo = await a.next();
  ok(echo.message === '[私聊] hello-bob', '发送者收到自己的回显');

  a.ws.close();
  b.ws.close();
  await sleep(200);
}

async function testDuplicateNameIsolation() {
  console.log('test: 同名多端不串号（P0 核心回归项）');
  const dup1 = await connect('dup_user');
  const dup2 = await connect('dup_user');
  const sender = await connect('sender_x');
  await sleep(300);
  dup1.drain();
  dup2.drain();
  sender.drain();

  sender.send({ key: ROOM, action: 'message', msgContent: 'to-both', toUser: 'dup_user' });
  const [r1, r2] = await Promise.all([dup1.next(), dup2.next()]);
  ok(r1.message === '[私聊] to-both', '同名第 1 个会话收到私聊');
  ok(r2.message === '[私聊] to-both', '同名第 2 个会话收到私聊');

  // 关键：断开较早的那个同名连接，新连接必须仍然可达
  dup1.ws.close();
  await sleep(600);
  dup2.drain();
  sender.drain();

  sender.send({ key: ROOM, action: 'message', msgContent: 'after-close', toUser: 'dup_user' });
  const r3 = await dup2.next(3000);
  ok(
    r3.message === '[私聊] after-close',
    `旧同名连接断开后，新连接仍能收到私聊（实际: ${r3.message}）`
  );

  dup2.ws.close();
  sender.ws.close();
  await sleep(200);
}

async function testOfflineNotice() {
  console.log('test: 目标离线提示');
  const a = await connect('alice3');
  await sleep(300);
  a.drain();

  a.send({ key: ROOM, action: 'message', msgContent: 'anyone?', toUser: 'nobody_xyz' });
  const resp = await a.next();
  ok(resp.code === 404, `离线目标返回 404（实际 code=${resp.code}）`);

  a.ws.close();
  await sleep(200);
}

async function testMalformedPayload() {
  console.log('test: 非法 JSON 不导致连接异常');
  const a = await connect('alice4');
  await sleep(300);
  a.drain();

  a.ws.send('this is not json');
  const resp = await a.next();
  ok(resp.code === -1, `非法负载返回错误响应且连接存活（实际 code=${resp.code}）`);

  // 连接仍然可用
  a.drain();
  a.send({ key: ROOM, action: 'message', msgContent: 'still-alive', toUser: '' });
  const echo = await a.next();
  ok(echo.message === 'still-alive', '非法负载后连接仍能正常收发');

  a.ws.close();
  await sleep(200);
}

async function main() {
  console.log(`目标: ${BASE}  房间: ${ROOM}\n`);
  try {
    await testRoomOrdering();
    await testDirectMessage();
    await testDuplicateNameIsolation();
    await testOfflineNotice();
    await testMalformedPayload();
  } catch (e) {
    console.log(`  [FAIL] 未捕获异常: ${e.message}`);
    failures++;
  }
  console.log(`\n${failures === 0 ? 'ALL PASSED' : 'FAILED'} (failures=${failures})`);
  process.exit(failures === 0 ? 0 : 1);
}

main();
