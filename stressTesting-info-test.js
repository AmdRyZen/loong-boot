import http from 'k6/http';
import { check, sleep } from 'k6';
import { htmlReport } from "https://raw.githubusercontent.com/benc-uk/k6-reporter/main/dist/bundle.js";
import { textSummary } from 'https://jslib.k6.io/k6-summary/0.0.1/index.js';


const baseUrl = "http://127.0.0.1:9090/";

const users = [
    { name: 'test001', password: '5d93ceb70e2bf5daa84ec3d0cd2c731a' },
    { name: 'test002', password: '5d93ceb70e2bf5daa84ec3d0cd2c731a' }
];

const loginUrl = baseUrl + 'api/v1/user/login?userId=999&passwd=123456';
const testUrls = [
    baseUrl + 'api/v1/user/999/getInfo'
    //baseUrl + 'config/info',
    // 其他需要测试的接口
];

export let options = {
    stages: [
       /* { duration: '1m', target: 5 }, // ramp-up to 5 users
        { duration: '5m', target: 5 }, // stay at 5 users
        { duration: '1m', target: 10 }, // ramp-up to 10 users
        { duration: '5m', target: 10 }, // stay at 10 users
        { duration: '1m', target: 0 }, // ramp-down to 0 users*/

        { duration: '1m', target: 2 }
    ],
    insecureSkipTLSVerify: true,
    thresholds: {
        http_req_duration: ['p(95)<2000'], // 95% 的请求在 2s 内完成
        'http_req_duration{status:200}': ['p(95)<1500'], // 95% 的 200 状态码的请求在 1.5s 内完成
    },
};

function login(user, retries = 3) {
    let headers = {
        'Content-Type': 'application/json',
        'X-API-UUID': '43ac6723-b248-4c79-bd1b-647fee7bbbde',
        'X-API-CLIENT': 'ios',
    };

    let body = JSON.stringify({ name: user.name, password: user.password, kaptchcate: user.kaptchcate });

    for (let i = 0; i < retries; i++) {
        let res = http.post(loginUrl, body, { headers });

        console.log('Login response: ', JSON.stringify(res.body, null, 2));

        if (check(res, {
            'status is 200': (r) => r.status === 200,
            'has data in response': (r) => r.json() !== undefined,
            'has token in data': (r) => r.json().token !== undefined,
        })) {
            return res.json().token;
        } else {
            sleep(1); // 等待一秒后重试
        }
    }

    throw new Error(`Failed to login after ${retries} retries`);
}

export function setup() {
    let tokens = {};

    users.forEach((user) => {
        tokens[user.name] = login(user);
    });

    return tokens;
}

export default function (tokens) {
    let user = users[Math.floor(Math.random() * users.length)];
    let token = tokens[user.name];

    // 定时刷新token，例如每隔10分钟刷新一次
    const refreshInterval = 60 * 10;
    if (__ITER % refreshInterval === 0) {
        token = login(user);
        tokens[user.name] = token;
    }

    // 并发地测试每个 URL
    let requests = testUrls.map((url) => {
        return new Promise((resolve) => {
            testApi(url, token);
            resolve();
        });
    });

    Promise.all(requests);
}

function testApi(url, token) {
    let headers = {
        'Content-Type': 'application/json',
        'X-API-UUID': '43ac6723-b248-4c79-bd1b-647fee7bbbde',
        'X-API-CLIENT': 'ios',
        'Authorization' : token
    };

    let body = JSON.stringify({});

    // 不同的接口  body 请求参数不一样
    if (url === baseUrl + 'api/v1/user/edit') {
        body = JSON.stringify({"id": 100000});
    }

    let res = http.post(url, body, { headers });

    if (res.status !== 200) {
        console.log(url, 'testApi response error : ', JSON.stringify(res.body, null, 2));
    }

    try {
        let responseJson = res.json();
        if (responseJson.code && responseJson.code !== 200) {
            //console.log(url, 'testApi response: ', JSON.stringify(responseJson.data, null, 2));
        }
    } catch (e) {
        console.log(url, 'Error parsing response: ', e.message);
        console.log(url, 'Raw response: ', res.body);
    }

    // console.log(url, 'testApi response: ', JSON.stringify(res.body, null, 2));

    check(res, {
        'status is 200': (r) => r.status === 200,
    });
}

// 输出详细的 HTML 报告
export function handleSummary(data) {
    return {
        "info-test-summary.html": htmlReport(data),
        "stdout": textSummary(data, { indent: ' ', enableColors: true }),
        "info-test-summary.json": JSON.stringify(data),
    };
}
