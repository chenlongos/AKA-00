import {io} from "socket.io-client";

export const socket = io();

export const sendAction = (action: string) => {
    console.log("ws send" + action);
    socket.emit('action', action);
}

export const resetCar = () => {
    socket.emit('reset_car_state');
}

export const getCarState = () => {
    socket.emit('get_car_state');
}