import threading

from app import create_app
from app.extensions import socketio

app = create_app()

def run_http():
    socketio.run(app, host="0.0.0.0", port=80)

def run_https():
    socketio.run(host='0.0.0.0', port=443, ssl_context=('/root/AKA-00/cert.pem', '/root/AKA-00/key.pem'))

if __name__ == '__main__':
    threading.Thread(target=run_http).start()
    # threading.Thread(target=run_https).start()