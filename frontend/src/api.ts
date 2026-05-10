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
};

// 摄像头
export const camera = {
    status: () => fetch("/api/camera/status").then(r => r.json()),
    open: () => fetch("/api/camera/open", {method: "POST"}).then(r => r.json()),
    close: () => fetch("/api/camera/close", {method: "POST"}).then(r => r.json()),
};

// Demo
export const demo = {
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