// 系统
export const system = {
    ip: () => fetch("/api/system/ip").then(r => r.json()),
    heartbeat: () => fetch("/api/system/heartbeat").then(r => r.json()),
};

// 电机
export const motor = {
    status: (timestamp?: number) => fetch(`/api/motor/status?timestamp=${timestamp ?? Date.now()}`).then(r => r.json()),
    direct: (left: number, right: number, duration: number = 0) =>
        fetch(`/api/motor/direct?left=${left}&right=${right}&duration=${duration}`).then(r => r.json()),
    action: (action: string, speed: number = 50, time: number = 0) =>
        fetch(`/api/control?action=${action}&speed=${speed}&time=${time}`).then(r => r.text()),
    rawCommand: (cmd: string) => fetch(`/api/motor/raw_command?cmd=${encodeURIComponent(cmd)}`).then(r => r.json()),
};

// 机械臂
export const arm = {
    angles: () => fetch("/api/arm/angles").then(r => r.json()),
    saveAngles: (driver: string, angles: object) =>
        fetch("/api/arm/angles", {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({driver, angles}),
        }).then(r => r.json()),
    preview: (driver: string, key: string, value: number, angles: object) =>
        fetch("/api/arm/angles/preview", {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({driver, key, value, angles}),
        }).then(r => r.json()),
};

// 底盘
export const base = {
    pwmChannels: () => fetch("/api/base/pwm_channels").then(r => r.json()),
    savePwmChannels: (pwm_channels: object) =>
        fetch("/api/base/pwm_channels", {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({pwm_channels}),
        }),
    reinitialize: () => fetch("/api/base/reinitialize", {method: "POST"}).then(r => r.json()),
};

// 摄像头
export const camera = {
    status: () => fetch("/api/camera/status").then(r => r.json()),
    open: () => fetch("/api/camera/open", {method: "POST"}).then(r => r.json()),
    close: () => fetch("/api/camera/close", {method: "POST"}).then(r => r.json()),
};

// Demo
export const demo = {
    list: () => fetch("/api/demo/list").then(r => r.json()),
    init: (name: string) => fetch("/api/demo/init", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({name}),
    }).then(r => r.json()),
    stop: () => fetch("/api/demo/stop", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({}),
    }).then(r => r.json()),
};

export const api = { system, motor, arm, base, camera, demo };

export type MotorStatus = { left: number; right: number };

// WebSocket 实时控制通道
export class ControlSocket {
    private ws: WebSocket | null = null;
    private reconnectTimer: number | null = null;
    private onStatusChange: ((connected: boolean) => void) | null = null;
    private onMotorStatus: ((status: MotorStatus) => void) | null = null;

    connect(onStatus?: (connected: boolean) => void, onMotorStatus?: (status: MotorStatus) => void) {
        this.onStatusChange = onStatus ?? null;
        this.onMotorStatus = onMotorStatus ?? null;
        this._doConnect();
    }

    private _doConnect() {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) return;
        const protocol = location.protocol === "https:" ? "wss:" : "ws:";
        const url = `${protocol}//${location.host}/ws/control`;
        this.ws = new WebSocket(url);
        this.ws.binaryType = "arraybuffer";

        this.ws.onopen = () => {
            this.onStatusChange?.(true);
            if (this.reconnectTimer) { clearInterval(this.reconnectTimer); this.reconnectTimer = null; }
        };

        this.ws.onmessage = (e) => {
            if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < 5) return;
            const dv = new DataView(e.data);
            if (dv.getUint8(0) !== 0xBB) return;
            const left = dv.getInt16(1, true) / 1000;
            const right = dv.getInt16(3, true) / 1000;
            this.onMotorStatus?.({ left, right });
        };

        this.ws.onclose = () => {
            this.onStatusChange?.(false);
            if (!this.reconnectTimer) {
                this.reconnectTimer = window.setInterval(() => this._doConnect(), 2000);
            }
        };

        this.ws.onerror = () => { this.ws?.close(); };
    }

    sendJoystick(x: number, y: number) {
        if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
        const buf = new ArrayBuffer(3);
        const dv = new DataView(buf);
        dv.setUint8(0, 0xAA);
        dv.setInt8(1, Math.max(-128, Math.min(127, x)));
        dv.setInt8(2, Math.max(-128, Math.min(127, y)));
        this.ws.send(buf);
    }

    close() {
        if (this.reconnectTimer) { clearInterval(this.reconnectTimer); this.reconnectTimer = null; }
        this.ws?.close();
        this.ws = null;
    }
}

export const controlSocket = new ControlSocket();