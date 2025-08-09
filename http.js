import http from 'k6/http';
import { sleep } from 'k6';

export let options = {
    vus: 100, // 虚拟用户数
    duration: '30s', // 测试持续时间
};

export default function() {
    http.get('http://127.0.0.1:9090/api/v1/openapi/getValue'); // 发起HTTP GET请求
    //http.get('http://127.0.0.1:9090/'); // 发起HTTP GET请求
    //sleep(1); // 可选：模拟用户思考时间，避免过度加载服务器
}