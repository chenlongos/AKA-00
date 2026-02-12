import threading

from app import create_app

app = create_app()

def run_http():
    app.run(host='0.0.0.0', port=80)


def run_https():
    app.run(host='0.0.0.0', port=443, ssl_context=('/root/AKA-00/cert.pem', '/root/AKA-00/key.pem'))

if __name__ == '__main__':
    threading.Thread(target=run_http).start()
    # threading.Thread(target=run_https).start()