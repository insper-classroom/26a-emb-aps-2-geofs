#!/usr/bin/env python3
"""
Ponte PC do Controle GeoFS.

Le o datagrama binario enviado pelo Pico pela serial USB e traduz em
comandos de teclado/mouse para o GeoFS (jogo de navegador).

Protocolo device -> PC (4 bytes, sync no inicio):
    [0xFF][TYPE][VAL_LO][VAL_HI]      (VAL = int16 little-endian)

    TYPE 0 = eixo X (roll)      -> mouse absoluto no eixo X
    TYPE 1 = eixo Y (pitch)     -> mouse absoluto no eixo Y
    TYPE 2 = throttle (0..1000) -> PageUp/PageDown ate convergir
    TYPE 3 = botao (id no VAL)  -> tecla do GeoFS
    TYPE 4 = gesto (id no VAL)  -> acao sem toque (fase 2)
    TYPE 6 = heartbeat do device

Protocolo PC -> device (1 byte): 0xA0 heartbeat, 0xA1 crash, 0xA2 stall.

Baseado no exemplo python-mouse do curso (mesma GUI + indicador de conexao).
"""

import sys
import glob
import time
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

import serial
import pyautogui
import tkinter as tk
from tkinter import ttk
from tkinter import messagebox

pyautogui.PAUSE = 0
pyautogui.FAILSAFE = False  # joystick pode levar o cursor ao canto da tela

if sys.platform.startswith("win"):
    import winreg

# ----------------------------------------------------------------------------
# Configuracao
# ----------------------------------------------------------------------------
BAUD = 115200

PKT_SYNC = 0xFF
TYPE_AXIS_X = 0
TYPE_AXIS_Y = 1
TYPE_THROTTLE = 2
TYPE_BUTTON = 3
TYPE_GESTURE = 4
TYPE_HEARTBEAT = 6

CMD_HEARTBEAT = 0xA0
CMD_CRASH = 0xA1
CMD_STALL = 0xA2

# Botoes -> teclas do GeoFS (ver controles em https://www.geo-fs.com/)
BTN_GEAR, BTN_FLAPS, BTN_BRAKE, BTN_VIEW, BTN_JOY = 0, 1, 2, 3, 4
KEYMAP = {
    BTN_GEAR: "g",   # trem de pouso
    BTN_FLAPS: "f",  # flaps
    BTN_BRAKE: "b",  # freio
    BTN_VIEW: "v",   # troca de camera
    # BTN_JOY tratado a parte (pausa o controle do mouse)
}

# Gestos (Edge Impulse via HC-SR04) -> acoes. Modelo de 2 classes: idle/hover.
GEST_IDLE, GEST_HOVER = 0, 3
GESTUREMAP = {
    GEST_HOVER: ("key", "a"),  # mao sobre o sensor -> autopilot (ajuste a tecla)
}

# Faixa do joystick: deflexao maxima em fracao da tela (a partir do centro).
MOUSE_RANGE_FRAC = 0.40
JOY_FULLSCALE = 2048.0  # |valor| maximo enviado pelo device

# Throttle: passo aproximado por toque de PageUp/PageDown (em %).
THROTTLE_STEP = 4
THROTTLE_DEADBAND = 3


# ----------------------------------------------------------------------------
# Tradutor: aplica os comandos recebidos ao GeoFS
# ----------------------------------------------------------------------------
class GeoFSController:
    def __init__(self):
        self.screen_w, self.screen_h = pyautogui.size()
        self.cx, self.cy = self.screen_w // 2, self.screen_h // 2
        self.range_x = self.screen_w * MOUSE_RANGE_FRAC
        self.range_y = self.screen_h * MOUSE_RANGE_FRAC
        self.joy_x = 0
        self.joy_y = 0
        self.throttle_target = 0
        self.throttle_est = 0
        self.control_active = True  # BTN_JOY pausa/retoma o controle do mouse

    # --- eixos do joystick: posicao ABSOLUTA do mouse (vale ate soltar) ---
    def set_axis(self, axis, value):
        if axis == TYPE_AXIS_X:
            self.joy_x = value
        else:
            self.joy_y = value
        if not self.control_active:
            return
        x = self.cx + (self.joy_x / JOY_FULLSCALE) * self.range_x
        y = self.cy + (self.joy_y / JOY_FULLSCALE) * self.range_y
        pyautogui.moveTo(int(x), int(y))

    # --- throttle: converge a estimativa local para o alvo do potenciometro ---
    def set_throttle(self, per_mille):
        self.throttle_target = max(0, min(100, per_mille // 10))
        diff = self.throttle_target - self.throttle_est
        if abs(diff) < THROTTLE_DEADBAND:
            return
        steps = int(round(diff / THROTTLE_STEP))
        key = "pageup" if steps > 0 else "pagedown"
        for _ in range(abs(steps)):
            pyautogui.press(key)
        self.throttle_est += steps * THROTTLE_STEP
        self.throttle_est = max(0, min(100, self.throttle_est))

    def press_button(self, btn_id):
        if btn_id == BTN_JOY:
            self.control_active = not self.control_active
            return
        key = KEYMAP.get(btn_id)
        if key:
            pyautogui.press(key)

    def do_gesture(self, gest_id):
        action = GESTUREMAP.get(gest_id)
        if action and action[0] == "key":
            pyautogui.press(action[1])


# ----------------------------------------------------------------------------
# Leitura/escrita da serial (em threads)
# ----------------------------------------------------------------------------
def reader_loop(ser, ctrl, on_status):
    """Le pacotes [0xFF TYPE LO HI] e aplica no GeoFS. on_status(text) p/ GUI."""
    while True:
        sync = ser.read(1)
        if not sync:
            continue
        if sync[0] != PKT_SYNC:
            continue
        data = ser.read(3)
        if len(data) < 3:
            continue
        ptype = data[0]
        value = int.from_bytes(data[1:3], byteorder="little", signed=True)
        try:
            if ptype in (TYPE_AXIS_X, TYPE_AXIS_Y):
                ctrl.set_axis(ptype, value)
            elif ptype == TYPE_THROTTLE:
                ctrl.set_throttle(value)
                on_status(f"throttle {value // 10}%")
            elif ptype == TYPE_BUTTON:
                ctrl.press_button(value)
                on_status(f"botao {value}")
            elif ptype == TYPE_GESTURE:
                ctrl.do_gesture(value)
                on_status(f"gesto {value}")
        except Exception:
            pass


def heartbeat_loop(ser):
    """Envia 0xA0 periodico p/ o device acender o LED de 'conectado'."""
    while True:
        try:
            ser.write(bytes([CMD_HEARTBEAT]))
        except Exception:
            return
        time.sleep(0.5)


# ----------------------------------------------------------------------------
# Feedback do jogo (ponto extra): um userscript no navegador le o estado do
# GeoFS (crash/stall) e chama http://127.0.0.1:8765/event?type=crash|stall.
# Aqui repassamos isso ao device como 0xA1/0xA2 -> vibracao/buzzer.
# Usa apenas a stdlib (http.server), sem dependencias extras.
# ----------------------------------------------------------------------------
class _FeedbackHandler(BaseHTTPRequestHandler):
    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_GET(self):
        q = parse_qs(urlparse(self.path).query)
        t = (q.get("type", [""])[0]).lower()
        byte = {"crash": CMD_CRASH, "stall": CMD_STALL}.get(t)
        if byte is not None and _Estado.ser is not None and _Estado.ser.is_open:
            try:
                _Estado.ser.write(bytes([byte]))
            except Exception:
                pass
        self.send_response(200)
        self._cors()
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(b"ok")

    def log_message(self, *args):
        pass  # silencia o log do http.server


def start_feedback_server(port=8765):
    try:
        srv = ThreadingHTTPServer(("127.0.0.1", port), _FeedbackHandler)
    except OSError:
        return  # porta ocupada; segue sem feedback do jogo
    threading.Thread(target=srv.serve_forever, daemon=True).start()


# ----------------------------------------------------------------------------
# Descoberta de portas seriais (igual ao python-mouse)
# ----------------------------------------------------------------------------
def serial_ports():
    if sys.platform.startswith("win"):
        ports = []
        try:
            key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                 r"HARDWARE\DEVICEMAP\SERIALCOMM")
            i = 0
            while True:
                try:
                    _, port, _ = winreg.EnumValue(key, i)
                    ports.append(port)
                    i += 1
                except OSError:
                    break
            winreg.CloseKey(key)
        except OSError:
            pass
        return sorted(ports, key=lambda p: int(p[3:]) if p[3:].isdigit() else 0)
    elif sys.platform.startswith("linux") or sys.platform.startswith("cygwin"):
        return glob.glob("/dev/tty[A-Za-z]*")
    elif sys.platform.startswith("darwin"):
        return glob.glob("/dev/tty.*")
    else:
        raise EnvironmentError("Plataforma nao suportada.")


# ----------------------------------------------------------------------------
# GUI (estende a do python-mouse: seletor de porta + indicador de conexao)
# ----------------------------------------------------------------------------
class _Estado:
    ser = None


def conectar_porta(port_name, root, botao, status_label, set_circ):
    if _Estado.ser is not None:
        if _Estado.ser.is_open:
            _Estado.ser.close()
        _Estado.ser = None
        botao.config(text="Conectar e Iniciar")
        status_label.config(text="Desconectado.", foreground="red")
        set_circ("red")
        return

    if not port_name:
        messagebox.showwarning("Aviso", "Selecione uma porta serial.")
        return

    try:
        ser = serial.Serial(port_name, BAUD, timeout=1)
    except Exception as e:
        messagebox.showerror("Erro de Conexao",
                             f"Nao foi possivel abrir {port_name}.\n{e}")
        return

    _Estado.ser = ser
    status_label.config(text=f"Conectado em {port_name}", foreground="green")
    set_circ("green")
    botao.config(text="Desconectar")

    ctrl = GeoFSController()

    def on_status(text):
        root.after(0, lambda: status_label.config(
            text=f"Conectado | {text}", foreground="green"))

    def loop():
        try:
            reader_loop(ser, ctrl, on_status)
        except Exception:
            pass
        finally:
            if ser.is_open:
                ser.close()
            _Estado.ser = None
            root.after(0, lambda: (
                status_label.config(text="Conexao encerrada.", foreground="red"),
                set_circ("red"),
                botao.config(text="Conectar e Iniciar"),
            ))

    threading.Thread(target=loop, daemon=True).start()
    threading.Thread(target=heartbeat_loop, args=(ser,), daemon=True).start()


def criar_janela():
    root = tk.Tk()
    root.title("Controle GeoFS - Ponte PC")
    root.geometry("520x340")
    root.resizable(False, False)

    dark_bg, dark_fg, accent = "#2e2e2e", "#ffffff", "#007acc"
    root.configure(bg=dark_bg)

    style = ttk.Style(root)
    style.theme_use("clam")
    style.configure("TFrame", background=dark_bg)
    style.configure("TLabel", background=dark_bg, foreground=dark_fg,
                    font=("Segoe UI", 11))
    style.configure("Accent.TButton", font=("Segoe UI", 12, "bold"),
                    foreground=dark_fg, background=accent, padding=6)
    style.map("Accent.TButton", background=[("active", "#005f9e")])
    style.configure("TCombobox", fieldbackground=dark_bg, background=dark_bg,
                    foreground=dark_fg, padding=4)
    style.map("TCombobox", fieldbackground=[("readonly", dark_bg)])

    frame = ttk.Frame(root, padding="20")
    frame.pack(expand=True, fill="both")

    ttk.Label(frame, text="Controle GeoFS",
              font=("Segoe UI", 16, "bold")).pack(pady=(0, 6))
    ttk.Label(frame, text="Joystick -> pitch/roll | Pot -> throttle | "
                          "Botoes/Gestos -> teclas").pack(pady=(0, 14))

    porta_var = tk.StringVar(value="")
    botao = ttk.Button(frame, text="Conectar e Iniciar", style="Accent.TButton",
                       command=lambda: conectar_porta(
                           porta_var.get(), root, botao, status_label, set_circ))
    botao.pack(pady=10)

    footer = tk.Frame(root, bg=dark_bg)
    footer.pack(side="bottom", fill="x", padx=10, pady=10)

    status_label = tk.Label(footer, text="Aguardando porta...",
                            font=("Segoe UI", 11), bg=dark_bg, fg=dark_fg)
    status_label.grid(row=0, column=0, sticky="w")

    portas = serial_ports()
    if portas:
        porta_var.set(portas[0])
    dropdown = ttk.Combobox(footer, textvariable=porta_var, values=portas,
                            state="normal", width=10)
    dropdown.grid(row=0, column=1, padx=10)

    def refresh():
        novas = serial_ports()
        dropdown["values"] = novas
        if novas and not porta_var.get():
            porta_var.set(novas[0])

    ttk.Button(footer, text="↺", width=3, command=refresh).grid(
        row=0, column=3, padx=(2, 0))

    canvas = tk.Canvas(footer, width=20, height=20, highlightthickness=0,
                       bg=dark_bg)
    circ = canvas.create_oval(2, 2, 18, 18, fill="red", outline="")
    canvas.grid(row=0, column=2, sticky="e")
    footer.columnconfigure(1, weight=1)

    def set_circ(cor):
        canvas.itemconfig(circ, fill=cor)

    # Servidor de feedback do jogo (crash/stall vindos do userscript do GeoFS)
    start_feedback_server()

    root.mainloop()


if __name__ == "__main__":
    criar_janela()
