import {io} from "socket.io-client";
import {useEffect, useState} from "react";

interface Message {
    data: string;
}

interface MyEventData {
    user: string;
    content: string;
}

const WebSocketTest = () => {
    const socket = io()
    const [log, setLog] = useState<string[]>([]);

    useEffect(() => {

        socket.on('connect', () => {
            console.log('✅ Connected, ID:', socket.id);
        });

        socket.on('connect_error', (err) => {
            console.error('❌ Connection error:', err);
        });

        // 👇 监听服务器发来的 'response' 事件
        socket.on('response', (data: Message) => {
            setLog(prev => [...prev, `[response] ${data.data}`]);
        });

        // 👇 监听服务器广播的 'my_response' 事件
        socket.on('my_response', (data: MyEventData) => {
            setLog(prev => [...prev, `[my_response] ${data.user}: ${data.content}`]);
        });

        // 清理监听器
        return () => {
            socket.off('response');
            socket.off('my_response');
        };
    }, []);

    // 🔹 发送 'message' 事件（字符串）
    const sendMessage = () => {
        console.log('>>> 发送 message 事件');
        socket.emit('message', 'Hello from frontend!');
    };

    // 🔹 发送 'my_event' 事件（JSON 对象）
    const sendCustomEvent = () => {
        console.log('>>> 发送 sendCustomEvent 事件');
        socket.emit('my_event', {
            user: 'Alice',
            content: 'This is a custom event!',
        });
    };

    return (
        <div style={{ padding: '20px' }}>
            <h2>Socket.IO 测试</h2>
            <button onClick={sendMessage}>发送 "message" 事件</button>
            <button onClick={sendCustomEvent}>发送 "my_event" 事件</button>

            <div style={{ marginTop: '20px', height: '300px', overflowY: 'auto', border: '1px solid #ccc', padding: '10px' }}>
                {log.map((line, i) => (
                    <div key={i}>{line}</div>
                ))}
            </div>
        </div>
    );
}
export default WebSocketTest;