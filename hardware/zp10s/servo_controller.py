#!/usr/bin/env python3
"""
ZP10S 舵机上位机 - macOS
协议格式: #<id>P<pos>T<time>!  波特率: 115200

参考文档
--------
- 舵机使用手册: hardware/众灵舵机使用手册-250508.pdf
- 指令集:       hardware/zp10s_commands.txt

使用步骤
--------
1. 安装依赖:  pip install pyserial
2. 硬件连接:  USB 数据线连接 PC 与舵机控制板
3. 跳线帽:    插到【下面】= USB 控制模式（实际运行时插上面 = UART 模式）
4. 运行程序:  python3 servo_controller.py  （从终端启动，可看到调试输出）
5. 选择串口:  macOS 串口名形如 /dev/tty.usbserial-XXXX，点"刷新"后选择，再点"连接"
6. 控制舵机:  拖动滑块或点"发送"控制角度；查询类指令会在日志显示返回值

注意事项
--------
- 角度控制指令（#000P1500T1000!）舵机不回复，日志只显示 [发]，属正常现象
- 查询指令（PRAD / PRTV 等）才有 [收] 返回
- 舵机出厂 ID=0，多个舵机需先用"读ID"/"设置ID"指令分配不同 ID（0/1/2）
- 位置范围 500-2500，中间位置 1500 对应约 90°
- 断电重启后跳线帽记得切换回对应模式
"""

import tkinter as tk
from tkinter import ttk, messagebox
import serial
import serial.tools.list_ports
import threading


def build_cmd(servo_id: int, pos: int, time_ms: int = 500) -> bytes:
    pos = max(500, min(2500, pos))
    return f"#{servo_id:03d}P{pos:04d}T{time_ms:04d}!".encode()


class ServoController:
    def __init__(self, root):
        self.root = root
        self.root.title("ZP10S 舵机控制器")
        self.ser = None
        self.num_servos = 3
        self._build_ui()

    def _build_ui(self):
        # 串口连接
        conn = ttk.LabelFrame(self.root, text="串口连接", padding=8)
        conn.pack(fill="x", padx=10, pady=5)

        ttk.Label(conn, text="串口:").grid(row=0, column=0)
        self.port_var = tk.StringVar()
        self.port_cb = ttk.Combobox(conn, textvariable=self.port_var, width=22)
        self.port_cb.grid(row=0, column=1, padx=4)
        ttk.Button(conn, text="刷新", command=self._refresh_ports).grid(row=0, column=2, padx=4)
        self.conn_btn = ttk.Button(conn, text="连接", command=self._toggle_connect)
        self.conn_btn.grid(row=0, column=3, padx=4)
        self.status_var = tk.StringVar(value="未连接")
        ttk.Label(conn, textvariable=self.status_var, foreground="red").grid(row=0, column=4, padx=8)

        # 舵机滑块控制
        ctrl = ttk.LabelFrame(self.root, text="舵机控制 (位置 500-2500)", padding=8)
        ctrl.pack(fill="x", padx=10, pady=5)

        self.pos_vars = []
        for i in range(self.num_servos):
            ttk.Label(ctrl, text=f"舵机 {i}").grid(row=i, column=0, padx=6)
            v = tk.IntVar(value=1500)
            self.pos_vars.append(v)
            ttk.Scale(ctrl, from_=500, to=2500, variable=v, orient="horizontal", length=280,
                      command=lambda _, idx=i: self._on_slide(idx)).grid(row=i, column=1, padx=6)
            ttk.Label(ctrl, textvariable=v, width=5).grid(row=i, column=2)
            ttk.Button(ctrl, text="发送", width=5,
                       command=lambda idx=i: self._send_single(idx)).grid(row=i, column=3, padx=4)

        time_row = ttk.Frame(ctrl)
        time_row.grid(row=self.num_servos, column=0, columnspan=4, pady=4, sticky="w")
        ttk.Label(time_row, text="运动时间(ms):").pack(side="left")
        self.time_var = tk.IntVar(value=500)
        ttk.Spinbox(time_row, from_=100, to=5000, increment=100, textvariable=self.time_var, width=6).pack(side="left", padx=4)
        ttk.Button(time_row, text="全部发送", command=self._send_all).pack(side="left", padx=6)
        ttk.Button(time_row, text="全部归中", command=self._center_all).pack(side="left")

        # 自定义指令
        cmd_frame = ttk.LabelFrame(self.root, text="自定义指令", padding=8)
        cmd_frame.pack(fill="x", padx=10, pady=5)

        # 快捷按钮行
        quick = ttk.Frame(cmd_frame)
        quick.pack(fill="x", pady=(0, 4))
        ttk.Label(quick, text="舵机ID:").pack(side="left")
        self.qid_var = tk.StringVar(value="000")
        ttk.Entry(quick, textvariable=self.qid_var, width=5).pack(side="left", padx=4)

        for label, suffix in [
            ("读角度", "PRAD"), ("读电压/温度", "PRTV"), ("读版本", "PVER"),
            ("停止", "PDST"), ("暂停", "PDPT"), ("继续", "PDCT"),
            ("归中偏差", "PSCK"), ("读ID", "PID"), ("读模式", "PMOD"),
        ]:
            ttk.Button(quick, text=label, width=9,
                       command=lambda s=suffix: self._send_quick(s)).pack(side="left", padx=2)

        # 手动输入行
        input_row = ttk.Frame(cmd_frame)
        input_row.pack(fill="x")
        ttk.Label(input_row, text="指令:").pack(side="left")
        self.cmd_var = tk.StringVar(value="#000PRAD!")
        ttk.Entry(input_row, textvariable=self.cmd_var, width=30).pack(side="left", padx=4)
        ttk.Button(input_row, text="发送", command=self._send_raw).pack(side="left", padx=4)
        ttk.Button(input_row, text="清空日志", command=self._clear_log).pack(side="right")

        # 通信日志
        log_frame = ttk.LabelFrame(self.root, text="通信日志", padding=8)
        log_frame.pack(fill="both", expand=True, padx=10, pady=5)
        self.log = tk.Text(log_frame, height=10, state="disabled", font=("Courier", 11))
        self.log.tag_config("blue",  foreground="#1565C0")
        self.log.tag_config("green", foreground="#2E7D32")
        self.log.tag_config("red",   foreground="#C62828")
        sb = ttk.Scrollbar(log_frame, command=self.log.yview)
        self.log.configure(yscrollcommand=sb.set)
        self.log.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")

        self._refresh_ports()

    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_cb["values"] = ports
        if ports:
            self.port_var.set(ports[0])

    def _toggle_connect(self):
        if self.ser and self.ser.is_open:
            self.ser.close()
            self.ser = None
            self.status_var.set("未连接")
            self.conn_btn.config(text="连接")
        else:
            try:
                self.ser = serial.Serial(self.port_var.get(), 115200, timeout=0.3)
                self.status_var.set("已连接")
                self.conn_btn.config(text="断开")
                self._log(f"[连接] {self.port_var.get()} 115200\n", "green")
                threading.Thread(target=self._read_loop, daemon=True).start()
            except Exception as e:
                messagebox.showerror("连接失败", str(e))

    def _read_loop(self):
        """后台持续读取串口返回，按 ! 分包，超时兜底显示原始数据"""
        buf = b""
        while self.ser and self.ser.is_open:
            try:
                data = self.ser.read(64)
                if data:
                    print(f"[串口原始收] {data!r}")
                    buf += data
                    while b"!" in buf:
                        idx = buf.index(b"!") + 1
                        msg = buf[:idx].decode(errors='replace')
                        buf = buf[idx:]
                        print(f"[收] {msg}")
                        self._log(f"[收] {msg}\n", "green")
                else:
                    if buf:
                        msg = buf.decode(errors='replace')
                        buf = b""
                        print(f"[收(无结尾)] {msg}")
                        self._log(f"[收] {msg}\n", "green")
            except Exception as e:
                print(f"[读取异常] {e}")
                break

    def _send_raw_bytes(self, data: bytes):
        if not (self.ser and self.ser.is_open):
            print("[错误] 未连接串口")
            self._log("[错误] 未连接串口\n", "red")
            return
        try:
            n = self.ser.write(data)
            ok = n == len(data)
            status = f"发送成功({n}字节)" if ok else f"发送失败(写入{n}/{len(data)}字节)"
            print(f"[发] {data!r}  {status}")
            self._log(f"[发] {data.decode(errors='replace')}  ← {status}\n", "blue" if ok else "red")
        except Exception as e:
            print(f"[发送异常] {e}")
            self._log(f"[发送异常] {e}\n", "red")

    def _send_raw(self):
        cmd = self.cmd_var.get().strip()
        if cmd:
            self._send_raw_bytes(cmd.encode())

    def _send_quick(self, suffix: str):
        sid = self.qid_var.get().zfill(3)
        cmd = f"#{sid}{suffix}!"
        self.cmd_var.set(cmd)
        self._send_raw_bytes(cmd.encode())

    def _on_slide(self, idx):
        if self.ser and self.ser.is_open:
            self._send_single(idx)

    def _send_single(self, idx):
        self._send_raw_bytes(build_cmd(idx, self.pos_vars[idx].get(), self.time_var.get()))

    def _send_all(self):
        for i in range(self.num_servos):
            self._send_single(i)

    def _center_all(self):
        for v in self.pos_vars:
            v.set(1500)
        self._send_all()

    def _log(self, msg: str, color: str = ""):
        self.root.after(0, self._append_log, msg, color)

    def _append_log(self, msg: str, color: str = ""):
        self.log.configure(state="normal")
        self.log.insert("end", msg, color if color else ())
        self.log.see("end")
        self.log.configure(state="disabled")

    def _clear_log(self):
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")


if __name__ == "__main__":
    root = tk.Tk()
    ServoController(root)
    root.mainloop()
