import {io} from "socket.io-client";

export const socket = io({
    path: "/socket.io",
    transports: ["polling"],
    upgrade: false,
    reconnection: true,
    reconnectionAttempts: Infinity,
    reconnectionDelay: 500,
    timeout: 20000
});

export const sendAction = (action: string) => {
    socket.emit('action', action);
}

export const resetCar = () => {
    socket.emit('reset_car_state');
}

export const getCarState = () => {
    socket.emit('get_car_state');
}

export const actInfer = (payload: Record<string, unknown>) => {
    socket.emit('act_infer', payload);
}

export const saveDataset = async (payload: Record<string, unknown>) => {
    const res = await fetch(`/api/dataset`, {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(payload)
    })
    return res.json()
}
