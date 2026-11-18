import {useEffect, useState} from "react";

/**
 * 页面可见性：标签页被切走、浏览器最小化、手机切到别的 App 时返回 false。
 *
 * 用途：摄像头 MJPEG 是长连接，浏览器在后台标签页里**不会主动断开**，车机会
 * 一直按 ~1Mbps 往外推流。隐藏时要把 <img> 的 src 换成空白图（实测仅卸载
 * <img> 元素浏览器不会取消请求），车机推流线程随之 write 失败退出
 * （display.set_streaming(false) 恢复屏显），回到页面时换回流地址即可恢复。
 */
export const usePageVisible = (): boolean => {
    const [visible, setVisible] = useState(() => !document.hidden);

    useEffect(() => {
        const sync = () => setVisible(!document.hidden);
        // pagehide/pageshow：iOS Safari 进后台、bfcache 前进后退时的兜底
        const onHide = () => setVisible(false);
        const onShow = () => setVisible(!document.hidden);
        document.addEventListener("visibilitychange", sync);
        window.addEventListener("pagehide", onHide);
        window.addEventListener("pageshow", onShow);
        return () => {
            document.removeEventListener("visibilitychange", sync);
            window.removeEventListener("pagehide", onHide);
            window.removeEventListener("pageshow", onShow);
        };
    }, []);

    return visible;
};
