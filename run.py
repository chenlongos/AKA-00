import os
import sys
import threading
import argparse

from app import create_app
from app.extensions import socketio

# 全局app变量，将在主块中初始化
app = None

def run_http():
    default_port = 5000 if os.name == "nt" else 80
    port = int(os.getenv("APP_HTTP_PORT", str(default_port)))
    socketio.run(app, host="0.0.0.0", port=port, allow_unsafe_werkzeug=True)

def run_https():
    cert_path = os.getenv("APP_CERT_PATH", "/root/AKA-00/cert.pem")
    key_path = os.getenv("APP_KEY_PATH", "/root/AKA-00/key.pem")
    if not (os.path.exists(cert_path) and os.path.exists(key_path)):
        return
    default_port = 5443 if os.name == "nt" else 443
    port = int(os.getenv("APP_HTTPS_PORT", str(default_port)))
    socketio.run(app, host='0.0.0.0', port=port, allow_unsafe_werkzeug=True, ssl_context=(cert_path, key_path))

if __name__ == '__main__':
    # 解析命令行参数
    parser = argparse.ArgumentParser(description='启动AKA-00服务器')
    parser.add_argument('--mode', choices=['hardware', 'simulation'], default='simulation',
                       help='运行模式: hardware(硬件模式) 或 simulation(模拟模式, 默认)')
    parser.add_argument('--port', type=int, help='HTTP端口 (覆盖默认值和APP_HTTP_PORT环境变量)')
    parser.add_argument('--https-port', type=int, help='HTTPS端口 (覆盖默认值和APP_HTTPS_PORT环境变量)')
    
    args = parser.parse_args()
    
    # 设置运行模式环境变量，供api.py等模块使用
    os.environ['AKA_MODE'] = args.mode
    print(f"[INFO] 运行模式: {args.mode}")
    
    # 如果指定了端口，覆盖环境变量
    if args.port:
        os.environ['APP_HTTP_PORT'] = str(args.port)
        print(f"[INFO] HTTP端口: {args.port}")
    if args.https_port:
        os.environ['APP_HTTPS_PORT'] = str(args.https_port)
        print(f"[INFO] HTTPS端口: {args.https_port}")
    
    # 创建Flask应用
    app = create_app()
    
    # 启动HTTP和HTTPS服务器
    threading.Thread(target=run_http).start()
    threading.Thread(target=run_https).start()