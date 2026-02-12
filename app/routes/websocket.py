from ..extensions import socketio
from flask_socketio import emit

# 监听客户端发送的 'message' 事件
@socketio.on('message')
def handle_message(data):
    print('收到消息: ' + data)
    # 将消息回传给发送者
    emit('response', {'data': '服务器已收到: ' + data})

# 监听自定义事件 'my_event'
@socketio.on('my_event')
def handle_my_custom_event(json):
    print('收到自定义事件数据: ' + str(json))
    # broadcast=True 表示广播给所有连接的客户端（群发）
    emit('my_response', json, broadcast=True)