"""自动重连底盘代理（对应 cpp/csrc AutoReconnectMotorPair，行为对齐）。

解决的问题：底盘 UART 在启动瞬间可能连不上（ESP32 还在上电、串口节点未就绪、
被瞬时占用等），旧实现把"启动时同步握手一次"当成硬依赖——失败即抛异常拖垮
整个服务（Python），或静默降级 Mock 后进程内永不再连真底盘（C++）。

本代理：
  - 构造永不抛异常 → 服务必然能起（未连上期间命令落到 Mock，无副作用）；
  - 后台线程按退避策略（0.5s→1s→…→30s 封顶）反复尝试连接真实底盘
    （TtPidChassis 内含 INIT/CONFIG 握手），连上即原子切换为真实驱动；
  - 已连接后周期 ping 探活，连续失败判定掉线 → 换回 Mock 并自动重连；
  - reinitialize() = 断开当前链路并立即完整重连（可被前端调用自愈）；
  - status() 暴露 backend/enabled/connected/state/attempts/error。

对外仍满足 MotorPairProtocol（set_speed / get_speeds / brake / sleep /
close / reinitialize / get_encoder），命令侧无感知。
"""

import threading
import time

# 重连退避 / 心跳参数（与 C++ 常量对齐）
_BACKOFF_BASE = 0.5
_BACKOFF_CAP = 30.0
_PING_INTERVAL = 2.0
_PING_FAILS_BEFORE_DROP = 2


class AutoReconnectMotorPair:
    """自动重连底盘代理（backend="tt_pid" 时后台建连；否则退化为纯 Mock）。"""

    def __init__(
        self,
        port: str = "/dev/ttyS1",
        backend: str = "tt_pid",
        baudrate: int = 115200,
        ppr: int = 4680,
        pwm_freq: int = 20000,
    ) -> None:
        from src.base_control.interfaces import MockMotorPair

        self._port = port
        self._backend = backend
        self._baudrate = baudrate
        self._ppr = ppr
        self._pwm_freq = pwm_freq

        self._enabled = backend == "tt_pid"
        self._mock = MockMotorPair()
        self._active: object = self._mock  # Mock 或真实 TtPidChassis
        self._connected = False
        self._attempts = 0
        self._error = ""

        self._lock = threading.Lock()        # 保护 _active/_connected/_attempts/_error
        self._attempt_lock = threading.Lock()  # 建连串行化（worker 与 reinitialize）
        self._stop = threading.Event()
        self._wake = threading.Event()       # request_reconnect / reinitialize 打断等待

        if self._enabled:
            self._thread = threading.Thread(target=self._worker, daemon=True, name="motor-reconnect")
            self._thread.start()
            print(f"[motor] auto-reconnect enabled (port={self._port} baud={self._baudrate})")
        else:
            print(f"[motor] backend={backend} → mock (auto-reconnect off)")

    # ── 命令侧（delegate 到当前驱动）──────────────────────────────
    def _driver(self):
        with self._lock:
            return self._active

    def set_speed(self, left: int, right: int) -> None:
        self._driver().set_speed(left, right)

    def get_speeds(self) -> tuple:
        return self._driver().get_speeds()

    def brake(self) -> None:
        self._driver().brake()

    def sleep(self) -> None:
        self._driver().sleep()

    def close(self) -> None:
        self._stop.set()
        self._wake.set()
        with self._attempt_lock:
            self._drop_current()
        thread = getattr(self, "_thread", None)
        if thread is not None and thread.is_alive():
            thread.join(timeout=3)

    def get_encoder(self) -> tuple:
        return self._driver().get_encoder()

    def _send_cmd_noresp(self, cmd: int, payload: bytes = b"") -> None:
        fn = getattr(self._driver(), "_send_cmd_noresp", None)
        if fn is not None:
            fn(cmd, payload)

    # ── 连接管理 ──────────────────────────────────────────────────

    def reinitialize(self) -> bool:
        """重新初始化底盘。

        已连上 → 原地重发 INIT/CONFIG（清 PID/编码器，不掉线不打断运行）；
        未连上或原地重发失败 → 断开并完整重连一次（自愈）。纯 mock 直接成功。
        """
        if not self._enabled:
            return True  # 纯 mock：沿用旧语义直接成功
        with self._attempt_lock:
            with self._lock:
                cur = self._active if self._connected else None
            if cur is not None and cur is not self._mock:
                reinit = getattr(cur, "reinitialize", None)
                try:
                    ok_inplace = bool(reinit()) if reinit is not None else False
                except Exception:
                    ok_inplace = False
                if ok_inplace:
                    self._wake.set()
                    return True  # 链路健康：原地 INIT/CONFIG 成功
                # 原地失败：仅当当前仍是同一条链才断开（防误杀并发重连的新链）
                self._drop_if_current(cur)
            ok = self._try_connect()
        self._wake.set()  # 让 worker 立即感知状态（连上→维护，失败→继续重试）
        return ok

    def request_reconnect(self) -> None:
        """打断退避/心跳并立刻重连（异步）。"""
        with self._attempt_lock:
            self._drop_current()
            self._wake.set()

    def status(self) -> dict:
        with self._lock:
            connected = self._connected
            enabled = self._enabled
            attempts = self._attempts
            error = self._error
        state = "connected" if connected else ("disabled" if not enabled else "reconnecting")
        return {
            "backend": self._backend,
            "enabled": enabled,
            "connected": connected,
            "state": state,
            "attempts": attempts,
            "error": error,
        }

    # ── 内部实现 ──────────────────────────────────────────────────

    def _worker(self) -> None:
        if not self._enabled:
            return
        while not self._stop.is_set():
            self._wake.clear()
            with self._lock:
                connected = self._connected

            if not connected:
                with self._attempt_lock:
                    if self._stop.is_set():
                        return
                    if self._try_connect():
                        connected = True
                if not connected:
                    with self._lock:
                        n = self._attempts
                    backoff = min(_BACKOFF_CAP, _BACKOFF_BASE * (2 ** max(0, n - 1)))
                    self._sleep_cancelable(backoff)
                    continue

            # 已连接：周期探活，连续失败判定掉线 → 回到连接循环
            fails = 0
            while not self._stop.is_set():
                if self._wake.is_set():  # request_reconnect / reinitialize
                    self._wake.clear()
                    break
                p = self._driver()  # 记住本次探测对象（掉线判定时只断它）
                alive = self._ping_driver(p)
                if not alive:
                    fails += 1
                    if fails >= _PING_FAILS_BEFORE_DROP:
                        print(f"[motor] heartbeat lost {fails} times → reconnecting")
                        with self._attempt_lock:
                            # 只断自己探测的这条链路，防误杀 reinitialize 刚建好的新链接
                            self._drop_if_current(p)
                        break
                else:
                    fails = 0
                self._sleep_cancelable(_PING_INTERVAL)

    def _try_connect(self) -> bool:
        try:
            from src.base_control.tt_pid import TtPidChassis

            chassis = TtPidChassis(
                port=self._port,
                baudrate=self._baudrate,
                ppr=self._ppr,
                pwm_freq=self._pwm_freq,
            )
        except Exception as e:
            with self._lock:
                self._attempts += 1
                self._error = str(e) or type(e).__name__
                attempt = self._attempts
                error = self._error
            print(f"[motor] connect attempt #{attempt} failed: {error}")
            return False

        with self._lock:
            old = self._active
            self._active = chassis
            self._connected = True
            self._attempts = 0
            self._error = ""
        if old is not self._mock:
            try:
                old.close()
            except Exception:
                pass
        print(f"[motor] ✓ real chassis connected ({self._port})")
        return True

    def _drop_current(self) -> None:
        with self._lock:
            old = self._active
            self._active = self._mock
            self._connected = False
        if old is not self._mock:
            print("[motor] link dropped")
            try:
                old.close()
            except Exception:
                pass

    def _drop_if_current(self, expected) -> bool:
        """仅当当前驱动仍是 expected 时才断开（防误杀并发重连建好的新链接）。"""
        with self._lock:
            if self._active is not expected:
                return False
            old = self._active
            self._active = self._mock
            self._connected = False
        if old is not self._mock:
            print("[motor] link dropped (heartbeat lost)")
            try:
                old.close()
            except Exception:
                pass
        return True

    def _ping_driver(self, driver) -> bool:
        if driver is self._mock:
            return True  # mock 恒活；仅对真实驱动做探活
        fn = getattr(driver, "ping", None)
        if fn is None:
            return True
        try:
            return bool(fn())
        except Exception:
            return False

    def _sleep_cancelable(self, secs: float) -> None:
        """分段睡眠，可被 stop/wake 打断（~≤200ms 响应）。"""
        end = time.monotonic() + max(0.0, secs)
        while not self._stop.is_set() and not self._wake.is_set():
            left = end - time.monotonic()
            if left <= 0:
                break
            time.sleep(min(left, 0.2))
