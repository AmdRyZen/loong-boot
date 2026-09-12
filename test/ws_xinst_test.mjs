// loong-boot 聊天室【跨实例私聊】回归测试（端到端）
//
// 覆盖：跨实例私聊双向投递 / 发送者回显 / 同实例私聊不重复投递 /
//       跨实例房间广播未被破坏 / 总线开启时不再误报「用户不在线」/
//       同名跨实例多端都能收到
//
// 为什么需要它：跨实例私聊失败是**静默**的 —— 消息就是不到，没有异常、
// 没有 5xx、指标也不涨（除非去看 ws_handler_exceptions_total）。
// 单实例的 ws_e2e_test.mjs 完全测不到这条路径。
//
// 前置条件（缺一不可）：
//   1. 本机 Redis 在跑（总线走 PUBLISH/SUBSCRIBE chat_cluster_bus）
//   2. 两个实例都开了 custom_config.enable_cluster_bus = true
//   3. 两个实例是【不同端口】的独立进程
//
// 起两个实例（用 /tmp 的配置副本，别改仓库里的 config.json）：
//   for p in 18092 18093; do
//     mkdir -p /tmp/xchat/i$p/log        # ← log/ 必须先建，否则启动即退（Log path does not exist）
//     cp config.json /tmp/xchat/i$p/config.json
//     sed -i '' "s/\"port\": 9090/\"port\": $p/" /tmp/xchat/i$p/config.json
//     sed -i '' 's/"enable_cluster_bus": false/"enable_cluster_bus": true/' /tmp/xchat/i$p/config.json
//     cp cmake-build-release/loong-boot /tmp/xchat/i$p/
//     (cd /tmp/xchat/i$p && nohup ./loong-boot config.json > stdout.log 2>&1 &)
//   done
//   redis-cli pubsub numsub chat_cluster_bus     # 应为 2（两个实例各一个订阅）
//
// ⚠️ 端口别用 9090~9093：本机 Kafka broker 占着 9092/9093（lsof -iTCP:9092 -sTCP:LISTEN
//    可确认），HTTP 端口与之冲突时实例直接 terminate（std::system_error）。
//
// 用法：
//   node test/ws_xinst_test.mjs [ws://127.0.0.1:18092/chat] [ws://127.0.0.1:18093/chat]

const A = process.argv[2] || 'ws://127.0.0.1:18092/chat';
const B = process.argv[3] || 'ws://127.0.0.1:18093/chat';
const ROOM = `xroom_${Date.now()}`;

let failures = 0;
const ok = (cond, msg) => {
  console.log(`  [${cond ? ' ok ' : 'FAIL'}] ${msg}`);
  if (!cond) failures++;
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function connect(base, name, room = ROOM) {
  return new Promise((resolve, reject) => {
    const url = `${base}?room_name=${encodeURIComponent(room)}&name=${encodeURIComponent(name)}`;
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
        next: (timeoutMs = 2000) =>
          new Promise((res, rej) => {
            if (inbox.length) return res(inbox.shift());
            const t = setTimeout(() => rej(new Error(`等待消息超时: ${name}`)), timeoutMs);
            waiters.push((d) => {
              clearTimeout(t);
              res(d);
            });
          }),
        close: () => ws.close(),
      });
    });
    ws.addEventListener('error', () => reject(new Error(`连接失败: ${name}`)));
  });
}

// 每个断言都必须把该到的消息【全部】消费掉，否则会顶掉后面断言的取件。
// （入群/退群公告按设计不跨实例广播，所以各实例只看到自己这边成员的公告。）

const alice = await connect(A, 'alice'); // 实例 A
const bob = await connect(B, 'bob'); // 实例 B
const carol = await connect(A, 'carol'); // 实例 A（用于「本地命中」不重复投递）
await sleep(500);
for (const c of [alice, bob, carol]) c.inbox.length = 0;
console.log(`实例A=${A}  ( alice, carol )`);
console.log(`实例B=${B}  ( bob )   房间=${ROOM}\n`);

// 1. 跨实例私聊 A -> B
alice.send({ action: 'message', toUser: 'bob', msgContent: 'cross-A2B' });
const bobGot = await bob.next();
ok(bobGot.message === '[私聊] cross-A2B', `跨实例私聊送达 B（实际: ${bobGot.message}）`);
const aliceEcho = await alice.next();
ok(aliceEcho.message === '[私聊] cross-A2B', `发送者 A 收到自己的回显（实际: ${aliceEcho.message}）`);

// 2. 反向 B -> A
bob.send({ action: 'message', toUser: 'alice', msgContent: 'cross-B2A' });
const aliceGot = await alice.next();
ok(aliceGot.message === '[私聊] cross-B2A', `跨实例私聊反向送达 A（实际: ${aliceGot.message}）`);
const bobEcho = await bob.next();
ok(bobEcho.message === '[私聊] cross-B2A', `发送者 B 收到自己的回显（实际: ${bobEcho.message}）`);

// 3. 同实例私聊：必须只收到一次（本地投递 + 集群广播不得叠加成两份）
alice.send({ action: 'message', toUser: 'carol', msgContent: 'local-A' });
const carolGot = await carol.next();
ok(carolGot.message === '[私聊] local-A', `同实例私聊送达（实际: ${carolGot.message}）`);
const aliceEcho2 = await alice.next();
ok(aliceEcho2.message === '[私聊] local-A', `发送者 A 收到回显（实际: ${aliceEcho2.message}）`);
await sleep(600);
ok(carol.inbox.length === 0, `同实例私聊不重复投递（剩余 ${carol.inbox.length} 条）`);

// 4. 跨实例房间广播未被破坏（私聊分流不能影响原路径）
alice.send({ action: 'message', msgContent: 'room-broadcast' });
const bobRoom = await bob.next();
ok(bobRoom.message === 'room-broadcast', `跨实例房间广播仍正常（实际: ${bobRoom.message}）`);
const aliceRoom = await alice.next(); // 发送者本人也是房间成员
ok(aliceRoom.message === 'room-broadcast', `发送者 A 收到房间广播（实际: ${aliceRoom.message}）`);

// 5. 两个实例上都没有这个昵称：总线开着就不能断言「不在线」（对方可能在第三个实例上）
alice.send({ action: 'message', toUser: 'nobody_at_all', msgContent: 'ghost' });
const ghost = await alice.next();
ok(ghost.message === '[私聊] ghost',
   `总线开启时本地无此人 -> 只回显不误报 404（实际: ${ghost.message} / code=${ghost.code}）`);

// 6. 同名跨实例多端：dave 同时在 A 和 B 上，两端都该收到。
//    这条锁住「昵称即身份、总是广播给其他实例」的设计选择 ——
//    若改成「本地命中就不广播」，B 上的 dave 会静默漏收。
const daveA = await connect(A, 'dave');
const daveB = await connect(B, 'dave');
await sleep(500);
daveA.inbox.length = 0;
daveB.inbox.length = 0;

carol.send({ action: 'message', toUser: 'dave', msgContent: 'multi-end' });
const daveA1 = await daveA.next();
ok(daveA1.message === '[私聊] multi-end', `同实例那一端收到（实际: ${daveA1.message}）`);
const daveB1 = await daveB.next();
ok(daveB1.message === '[私聊] multi-end', `跨实例同名那一端也收到（实际: ${daveB1.message}）`);

for (const c of [alice, bob, carol, daveA, daveB]) c.close();
console.log(`\n${failures === 0 ? 'ALL PASSED' : 'FAILED'} (failures=${failures})`);
process.exit(failures === 0 ? 0 : 1);
