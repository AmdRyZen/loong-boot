import ws from 'k6/ws';
import { check } from 'k6';

// k6 run -u 10 ./websocket.js
export let options = {
    vus: 10, // 同时运行的虚拟用户数量
    duration: '15s', // 测试持续时间，这里设置为30秒
};

export default function () {
    const url = 'ws://127.0.0.1:9090/chat'; // WebSocket URL
    const params = { tags: { my_tag: 'websocket' } };

    const message = JSON.stringify({
        "key": "aa",
        "action": "message",
        "msgContent": "hahahahh"
        /* "Name": "Liming",
        "Age": 26,
        "Language": [
            "C++",
            "Java"
        ],
        "E-mail": {
            "Netease": "lmshao@163.com",
            "Hotmail": "liming.shao@hotmail.com"
        }*/
    });

    const res = ws.connect(url, params, function (socket) {
        socket.on('open', function open() {
            console.log('connected');
            socket.send(message);
        });

        socket.on('message', function (data) {
            socket.send(message);

            // 检查是否是最后一次迭代并关闭连接
           /* if (__ITER === __VU - 1) {
                console.log('Closing the socket after last iteration');
                socket.close();
            }*/
        });

        socket.on('close', function () {
            console.log('disconnected');
        });

        socket.on('error', function (e) {
            console.log('An error occurred:', e.error());
        });

        // 在一定时间后手动关闭连接
       /* socket.setTimeout(function () {
            console.log('Closing the socket after timeout');
            socket.close();
        }, 10000); // 10秒后关闭连接*/
    });

    check(res, { 'status is 101': (r) => r && r.status === 101 });

    // 添加 sleep 以模拟持续的连接活动
    //sleep(1);
}
