#!/usr/bin/env python3
"""Standalone loopback MCP host for an already-running Elite virtual VR session.

Uses the external Mod Linker transport C ABI and immutable TestHarness JSON
protocol. Never injects game code or installs/stops SteamVR. Keyboard delivery
uses US physical scancodes; queued SendInput events do not prove game behavior.
Windows x64 and a non-elevated interactive session shared with the game are required.
--dry-run writes nothing and loads/executes nothing; --self-test is offline.
"""
import argparse
import base64
from collections import deque
import ctypes as ct
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import time
import unittest
from unittest import mock
import zlib


KEYS = dict(zip(('UP','DOWN','LEFT','RIGHT','ENTER','ESCAPE','TAB','SPACE','W','A','S','D','Q','E',
                 'R','F','C','V','X','Z','F8','F9','0','1','2','3','4','5','6','7','8','9'),
                (0x48,0x50,0x4b,0x4d,0x1c,1,0x0f,0x39,0x11,0x1e,0x1f,0x20,0x10,0x12,
                 0x13,0x21,0x2e,0x2f,0x2d,0x2c,0x42,0x43,0x0b,2,3,4,5,6,7,8,9,10)))
KEYS.update({'SHIFT':0x2a,'BACKSPACE':0x0e,'COMMA':0x33,'PERIOD':0x34})
TOOLS = {'get_state','xr_sim_status','xr_sim_start','xr_sim_stop','xr_sim_set_pose',
         'xr_sim_set_eyes','xr_sim_play','xr_sim_capture','keyboard_input','keyboard_sequence',
         'mouse_click','capture_window','xr_sim_idle'}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def fields(value, allowed, required=()):
    require(type(value) is dict and set(value) <= set(allowed) and set(required) <= set(value),
            'Missing or unsupported argument fields')


def integer(value, low, high):
    require(type(value) is int and low <= value <= high, 'Integer outside permitted bounds')
    return value


def smooth_duration(value):
    """Validate an optional existing driver transition; omission keeps immediate delivery."""
    if 'smooth_seconds' in value:
        duration = value['smooth_seconds']
        require(type(duration) in (int,float) and math.isfinite(duration) and 0 <= duration <= 60,
                'smooth_seconds must be finite seconds in 0..60')


def idle_arguments(value):
    """Validate bounded idle actions before runtime calls or lease mutation."""
    fields(value,('action','duration_ms','seed'),('action',))
    action = value['action']
    require(type(action) is str and action in ('start','renew','stop'),'Unknown idle action')
    if action == 'start':
        fields(value,('action','duration_ms','seed'),('action','duration_ms','seed'))
        integer(value['duration_ms'],1,60000)
        integer(value['seed'],0,2**32-1)
    else:
        fields(value,('action',),('action',))
    return action


def pose(value, extra=()):
    fields(value, ('position','orientation','tracked',*extra))
    if 'tracked' in value:
        require(type(value['tracked']) is bool, 'tracked must be boolean')
    for name, count in (('position',3),('orientation',4)):
        if name in value:
            vector = value[name]
            require(type(vector) is list and len(vector) == count and
                    all(type(x) in (int,float) and math.isfinite(x) for x in vector), 'Invalid pose vector')
    if 'orientation' in value:
        require(abs(sum(x*x for x in value['orientation']) - 1) < .01, 'Quaternion must be normalized xyzw')
    if 'device' in extra:
        require(value.get('device') in ('head','left','right'), 'Unknown device')


def key_steps(value, sequence=False):
    fields(value, ('steps',) if sequence else ('keys','duration_ms'), ('steps',) if sequence else ('keys','duration_ms'))
    steps = value['steps'] if sequence else [value]
    require(type(steps) is list and 1 <= len(steps) <= 32, 'Expected 1..32 keyboard steps')
    total = 0
    for step in steps:
        fields(step, ('keys','duration_ms','gap_ms') if sequence else ('keys','duration_ms'), ('keys','duration_ms'))
        keys = step['keys']
        require(type(keys) is list and 1 <= len(keys) <= 8 and all(type(k) is str and k in KEYS for k in keys)
                and len(set(keys)) == len(keys), 'Expected 1..8 distinct whitelisted keys')
        total += integer(step['duration_ms'],1,2000) + integer(step.get('gap_ms',0),0,1000)
    require(total <= 10000, 'Keyboard sequence exceeds 10 seconds')
    return steps


class KeyboardInput(ct.Structure):
    _fields_ = [('wVk',ct.c_uint16),('wScan',ct.c_uint16),('dwFlags',ct.c_uint32),
                ('time',ct.c_uint32),('dwExtraInfo',ct.c_uint64)]


class MouseInput(ct.Structure):
    _fields_ = [('dx',ct.c_int32),('dy',ct.c_int32),('mouseData',ct.c_uint32),('dwFlags',ct.c_uint32),
                ('time',ct.c_uint32),('dwExtraInfo',ct.c_uint64)]


class InputUnion(ct.Union):
    _fields_ = [('ki',KeyboardInput),('mi',MouseInput)]


class Input(ct.Structure):
    _anonymous_ = ('data',)
    _fields_ = [('type',ct.c_uint32),('data',InputUnion)]


class Point(ct.Structure):
    _fields_ = [('x',ct.c_int32),('y',ct.c_int32)]


class Rect(ct.Structure):
    _fields_ = [('left',ct.c_int32),('top',ct.c_int32),('right',ct.c_int32),('bottom',ct.c_int32)]


class WindowPlacement(ct.Structure):
    _fields_ = [('length',ct.c_uint32),('flags',ct.c_uint32),('show',ct.c_uint32),
                ('minimum',Point),('maximum',Point),('normal',Rect)]


class BitmapHeader(ct.Structure):
    _fields_ = [('size',ct.c_uint32),('width',ct.c_int32),('height',ct.c_int32),('planes',ct.c_uint16),
                ('bits',ct.c_uint16),('compression',ct.c_uint32),('image_size',ct.c_uint32),
                ('xppm',ct.c_int32),('yppm',ct.c_int32),('colors',ct.c_uint32),('important',ct.c_uint32)]


class BitmapInfo(ct.Structure):
    _fields_ = [('header',BitmapHeader),('colors',ct.c_uint32*3)]


def capture_size(width,height,edge):
    require(0 < width <= 16384 and 0 < height <= 16384,'Invalid game client dimensions')
    scale = min(1,edge/max(width,height))
    return max(1,round(width*scale)),max(1,round(height*scale))


def client_point(x,y,width,height):
    require(all(type(v) in (int,float) and math.isfinite(v) and 0 <= v <= 1 for v in (x,y)),
            'Mouse coordinates must be normalized finite client coordinates in 0..1')
    require(width > 0 and height > 0,'Empty client area')
    return round(x*(width-1)),round(y*(height-1))


def desktop_png(bgra,width,height):
    """Encode top-down GDI BGRA as opaque RGBA, rejecting black independently of alpha."""
    require(len(bgra) == width*height*4,'Invalid desktop pixel layout')
    require(any(bgra[0::4]) or any(bgra[1::4]) or any(bgra[2::4]),'Game client capture is uniformly RGB-black')
    rgba = bytearray(len(bgra))
    rgba[0::4],rgba[1::4],rgba[2::4] = bgra[2::4],bgra[1::4],bgra[0::4]
    rgba[3::4] = b'\xff'*(width*height)
    stride = width*4
    raw = b''.join(b'\0'+rgba[y*stride:(y+1)*stride] for y in range(height))
    def chunk(kind,payload):
        return struct.pack('>I',len(payload))+kind+payload+struct.pack('>I',zlib.crc32(kind+payload))
    return (b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,6,0,0,0))
            +chunk(b'IDAT',zlib.compress(raw,3))+chunk(b'IEND',b''))


class Windows:
    """Narrow Win32 bindings, without command lines, privilege changes or global hotkeys."""
    def __init__(self):
        require(sys.platform == 'win32' and ct.sizeof(ct.c_void_p) == 8 and ct.sizeof(Input) == 40,
                'Windows x64 INPUT ABI required')
        self.k = ct.WinDLL('kernel32',use_last_error=True)
        self.u = ct.WinDLL('user32',use_last_error=True)
        self.a = ct.WinDLL('advapi32',use_last_error=True)
        self.g = ct.WinDLL('gdi32',use_last_error=True)
        self.callback = ct.WINFUNCTYPE(ct.c_int32,ct.c_void_p,ct.c_ssize_t)
        bindings = (
            (self.k,'OpenProcess',ct.c_void_p,(ct.c_uint32,ct.c_int32,ct.c_uint32)),
            (self.k,'CloseHandle',ct.c_int32,(ct.c_void_p,)),
            (self.k,'QueryFullProcessImageNameW',ct.c_int32,(ct.c_void_p,ct.c_uint32,ct.c_wchar_p,ct.POINTER(ct.c_uint32))),
            (self.k,'GetProcessTimes',ct.c_int32,(ct.c_void_p,*([ct.POINTER(ct.c_uint64)]*4))),
            (self.k,'GetExitCodeProcess',ct.c_int32,(ct.c_void_p,ct.POINTER(ct.c_uint32))),
            (self.k,'ProcessIdToSessionId',ct.c_int32,(ct.c_uint32,ct.POINTER(ct.c_uint32))),
            (self.k,'GetCurrentProcess',ct.c_void_p,()), (self.k,'GetCurrentThreadId',ct.c_uint32,()),
            (self.a,'OpenProcessToken',ct.c_int32,(ct.c_void_p,ct.c_uint32,ct.POINTER(ct.c_void_p))),
            (self.a,'GetTokenInformation',ct.c_int32,(ct.c_void_p,ct.c_uint32,ct.c_void_p,ct.c_uint32,ct.POINTER(ct.c_uint32))),
            (self.u,'GetForegroundWindow',ct.c_void_p,()),
            (self.u,'GetWindowThreadProcessId',ct.c_uint32,(ct.c_void_p,ct.POINTER(ct.c_uint32))),
            (self.u,'EnumWindows',ct.c_int32,(self.callback,ct.c_ssize_t)),
            (self.u,'IsWindowVisible',ct.c_int32,(ct.c_void_p,)),
            (self.u,'ShowWindow',ct.c_int32,(ct.c_void_p,ct.c_int)),
            (self.u,'SetForegroundWindow',ct.c_int32,(ct.c_void_p,)),
            (self.u,'BringWindowToTop',ct.c_int32,(ct.c_void_p,)),
            (self.u,'AttachThreadInput',ct.c_int32,(ct.c_uint32,ct.c_uint32,ct.c_int32)),
            (self.u,'SendInput',ct.c_uint32,(ct.c_uint32,ct.POINTER(Input),ct.c_int)),
            (self.u,'GetClientRect',ct.c_int32,(ct.c_void_p,ct.POINTER(Rect))),
            (self.u,'GetWindowRect',ct.c_int32,(ct.c_void_p,ct.POINTER(Rect))),
            (self.u,'GetWindowPlacement',ct.c_int32,(ct.c_void_p,ct.POINTER(WindowPlacement))),
            (self.u,'ClientToScreen',ct.c_int32,(ct.c_void_p,ct.POINTER(Point))),
            (self.u,'IsIconic',ct.c_int32,(ct.c_void_p,)),
            (self.u,'IsChild',ct.c_int32,(ct.c_void_p,ct.c_void_p)),
            (self.u,'GetClassNameW',ct.c_int32,(ct.c_void_p,ct.c_wchar_p,ct.c_int)),
            (self.u,'GetSystemMetrics',ct.c_int32,(ct.c_int,)),
            (self.u,'WindowFromPoint',ct.c_void_p,(Point,)),
            (self.u,'GetCursorPos',ct.c_int32,(ct.POINTER(Point),)),
            (self.u,'SetCursorPos',ct.c_int32,(ct.c_int,ct.c_int)),
            (self.u,'SetThreadDpiAwarenessContext',ct.c_void_p,(ct.c_void_p,)),
            (self.u,'GetDC',ct.c_void_p,(ct.c_void_p,)),
            (self.u,'ReleaseDC',ct.c_int,(ct.c_void_p,ct.c_void_p)),
            (self.g,'CreateCompatibleDC',ct.c_void_p,(ct.c_void_p,)),
            (self.g,'DeleteDC',ct.c_int32,(ct.c_void_p,)),
            (self.g,'CreateDIBSection',ct.c_void_p,(ct.c_void_p,ct.POINTER(BitmapInfo),ct.c_uint32,
                                                 ct.POINTER(ct.c_void_p),ct.c_void_p,ct.c_uint32)),
            (self.g,'SelectObject',ct.c_void_p,(ct.c_void_p,ct.c_void_p)),
            (self.g,'DeleteObject',ct.c_int32,(ct.c_void_p,)),
            (self.g,'BitBlt',ct.c_int32,(ct.c_void_p,ct.c_int,ct.c_int,ct.c_int,ct.c_int,ct.c_void_p,ct.c_int,ct.c_int,ct.c_uint32)),
            (self.g,'StretchBlt',ct.c_int32,(ct.c_void_p,ct.c_int,ct.c_int,ct.c_int,ct.c_int,
                                          ct.c_void_p,ct.c_int,ct.c_int,ct.c_int,ct.c_int,ct.c_uint32)),
            (self.g,'SetStretchBltMode',ct.c_int,(ct.c_void_p,ct.c_int)),
            (self.g,'GdiFlush',ct.c_int32,()))
        for library, name, result, arguments in bindings:
            fn = getattr(library,name)
            fn.restype, fn.argtypes = result,list(arguments)
        self.u.OpenInputDesktop.argtypes,self.u.OpenInputDesktop.restype = [ct.c_uint32,ct.c_int32,ct.c_uint32],ct.c_void_p
        self.u.CloseDesktop.argtypes,self.u.CloseDesktop.restype = [ct.c_void_p],ct.c_int32
        self.u.GetUserObjectInformationW.argtypes,self.u.GetUserObjectInformationW.restype = [ct.c_void_p,ct.c_int,ct.c_void_p,ct.c_uint32,ct.POINTER(ct.c_uint32)],ct.c_int32
        require(bool(self.u.SetThreadDpiAwarenessContext(ct.c_void_p(-4))), 'Per-monitor physical-pixel DPI context unavailable')

    def interactive(self):
        desktop = self.u.OpenInputDesktop(0,False,1)
        require(bool(desktop),'Interactive input desktop unavailable')
        try:
            name,needed = ct.create_unicode_buffer(256),ct.c_uint32()
            require(self.u.GetUserObjectInformationW(desktop,2,name,ct.sizeof(name),ct.byref(needed)),
                    'Input desktop name unavailable')
            require(name.value.casefold() == 'default','Secure/locked input desktop refused')
        finally:
            self.u.CloseDesktop(desktop)

    def session(self,pid):
        result = ct.c_uint32()
        require(self.k.ProcessIdToSessionId(pid,ct.byref(result)), 'Unable to query process session')
        return result.value

    def environment(self):
        current_session = self.session(os.getpid())
        require(current_session > 0, 'Interactive user session required')
        self.interactive()
        token, returned, elevated = ct.c_void_p(),ct.c_uint32(),ct.c_uint32()
        require(self.a.OpenProcessToken(self.k.GetCurrentProcess(),8,ct.byref(token)), 'Token query failed')
        try:
            require(self.a.GetTokenInformation(token,20,ct.byref(elevated),4,ct.byref(returned)), 'Elevation query failed')
            require(not elevated.value, 'Elevated host refused')
        finally:
            self.k.CloseHandle(token)
        # A visible target window is also required before any keyboard operation.
        return current_session

    def identity(self,pid):
        handle = self.k.OpenProcess(0x1000,False,pid)
        if not handle:
            require(ct.get_last_error() in (87,1168), 'Unable to query target process identity')
            return None
        try:
            code = ct.c_uint32()
            require(self.k.GetExitCodeProcess(handle,ct.byref(code)), 'Process status query failed')
            if code.value != 259:
                return None
            times = [ct.c_uint64() for _ in range(4)]
            require(self.k.GetProcessTimes(handle,*(ct.byref(x) for x in times)), 'Process creation query failed')
            image, size = ct.create_unicode_buffer(32768),ct.c_uint32(32768)
            require(self.k.QueryFullProcessImageNameW(handle,0,image,ct.byref(size)), 'Process image query failed')
            return {'pid':pid,'creation_ticks':str(times[0].value),'image':image.value,'session':self.session(pid)}
        finally:
            self.k.CloseHandle(handle)

    def window_pid(self,hwnd):
        pid = ct.c_uint32()
        self.u.GetWindowThreadProcessId(hwnd,ct.byref(pid))
        return pid.value

    def foreground_pid(self):
        return self.window_pid(self.u.GetForegroundWindow())

    def top_windows(self):
        windows = []
        @self.callback
        def visit(hwnd,_):
            windows.append(hwnd)
            return True
        require(self.u.EnumWindows(visit,0), 'Window enumeration failed')
        return windows

    def primary(self,pid,include_iconic=False):
        """Prefer the game's main class; restore candidates may include minimized windows."""
        candidates = []
        for hwnd in self.top_windows():
            if self.u.IsWindowVisible(hwnd) and self.window_pid(hwnd) == pid:
                iconic = bool(self.u.IsIconic(hwnd))
                if iconic and not include_iconic:
                    continue
                rect = Rect()
                known_client = self.u.GetClientRect(hwnd,ct.byref(rect)) and rect.right > 0 and rect.bottom > 0
                area = rect.right*rect.bottom if known_client else 0
                if iconic:
                    placement = WindowPlacement()
                    placement.length = ct.sizeof(WindowPlacement)
                    if self.u.GetWindowPlacement(hwnd,ct.byref(placement)):
                        restored = placement.normal
                        # Restored outer bounds rank candidates only; actual client bounds are read after restore.
                        area = max(area,max(0,restored.right-restored.left)*max(0,restored.bottom-restored.top))
                if known_client or iconic:
                    name = ct.create_unicode_buffer(256)
                    self.u.GetClassNameW(hwnd,name,len(name))
                    candidates.append((name.value == 'FrontierDevelopmentsAppWinClass',area,hwnd))
        require(bool(candidates),'No visible game restore candidate' if include_iconic else 'No visible nonminimized game client window')
        return max(candidates,key=lambda item:item[:2])[2]

    def client(self,pid):
        hwnd = self.primary(pid)
        rect,origin = Rect(),Point()
        require(self.u.GetClientRect(hwnd,ct.byref(rect)) and self.u.ClientToScreen(hwnd,ct.byref(origin)),
                'Game client geometry unavailable')
        width,height = rect.right-rect.left,rect.bottom-rect.top
        capture_size(width,height,2048)
        return {'hwnd':int(hwnd),'width':width,'height':height,'screen_left':origin.x,'screen_top':origin.y}

    def describe(self,pid):
        foreground = self.u.GetForegroundWindow()
        name = ct.create_unicode_buffer(256)
        self.u.GetClassNameW(foreground,name,len(name))
        result = {'foreground_hwnd':int(foreground or 0),'foreground_class':name.value,
                  'virtual_desktop':[self.u.GetSystemMetrics(i) for i in (76,77,78,79)]}
        try: result['target_client'] = self.client(pid)
        except ValueError as error: result['target_client_error'] = str(error)
        return result

    def focus(self,pid,verify):
        verify()
        target = self.primary(pid,include_iconic=True)
        for attempt in range(6):
            verify()
            require(self.window_pid(target) == pid,'Target window identity changed')
            if self.u.IsIconic(target):
                self.u.ShowWindow(target,9)  # Restore before any foreground shortcut, including same-PID focus.
                if self.u.IsIconic(target):
                    time.sleep(.05)
                    continue
            target = self.primary(pid)
            require(self.window_pid(target) == pid and not self.u.IsIconic(target),'Restored target window changed')
            if self.u.GetForegroundWindow() == target:
                return
            attached = []
            current = self.k.GetCurrentThreadId()
            try:
                if attempt:
                    for hwnd in (self.u.GetForegroundWindow(),target):
                        thread = self.u.GetWindowThreadProcessId(hwnd,None)
                        if thread and thread != current and thread not in attached and self.u.AttachThreadInput(current,thread,True):
                            attached.append(thread)
                self.u.BringWindowToTop(target)
                self.u.SetForegroundWindow(target)
            finally:
                for thread in reversed(attached):
                    self.u.AttachThreadInput(current,thread,False)
            time.sleep(.05)
        verify()
        require(self.window_pid(target) == pid and self.u.GetForegroundWindow() == target
                and not self.u.IsIconic(target),'Unable to focus the restored primary game window')

    def focused_client(self,pid,verify):
        """Validate identity, exact restored foreground HWND and client geometry."""
        verify()
        require(self.foreground_pid() == pid,'Game lost foreground focus')
        client = self.client(pid)
        require(self.u.GetForegroundWindow() == client['hwnd'] and not self.u.IsIconic(client['hwnd']),
                'Primary game client is not the restored foreground window')
        return client

    def point_visible(self,point):
        x,y,width,height = (self.u.GetSystemMetrics(i) for i in (76,77,78,79))
        return width > 0 and height > 0 and x <= point[0] < x+width and y <= point[1] < y+height

    def visible_client(self,pid,verify):
        """Require full desktop visibility for capture; top-level overlaps fail closed."""
        client = self.focused_client(pid,verify)
        left,top,width,height = (client[k] for k in ('screen_left','screen_top','width','height'))
        x,y,w,h = (self.u.GetSystemMetrics(i) for i in (76,77,78,79))
        require(w > 0 and h > 0 and x <= left and y <= top and left+width <= x+w and top+height <= y+h,
                'Game client extends outside the visible desktop')
        found = False
        for hwnd in self.top_windows():
            if hwnd == client['hwnd']:
                found = True
                break
            if self.u.IsWindowVisible(hwnd) and not self.u.IsIconic(hwnd):
                rect = Rect()
                require(self.u.GetWindowRect(hwnd,ct.byref(rect)),'Occlusion geometry unavailable')
                require(rect.right <= left or rect.left >= left+width or rect.bottom <= top or rect.top >= top+height,
                        'Visible window overlaps game client; unobscured pixels cannot be established')
        require(found,'Game window disappeared from desktop order')
        return client

    def cursor(self):
        point = Point()
        require(self.u.GetCursorPos(ct.byref(point)),'Cursor observation unavailable')
        return point.x,point.y

    def mouse_button(self,up):
        packet = Input()
        packet.type,packet.mi.dwFlags = 0,4 if up else 2
        return self.u.SendInput(1,ct.byref(packet),ct.sizeof(Input)) == 1

    def capture(self,pid,edge,verify):
        self.focus(pid,verify)
        client = self.visible_client(pid,verify)
        width,height = capture_size(client['width'],client['height'],edge)
        desktop = memory = bitmap = previous = None
        try:
            desktop = self.u.GetDC(None)
            require(bool(desktop),'Desktop DC unavailable')
            memory = self.g.CreateCompatibleDC(desktop)
            require(bool(memory),'Capture memory DC unavailable')
            info,bits = BitmapInfo(),ct.c_void_p()
            info.header.size,info.header.width,info.header.height = 40,width,-height
            info.header.planes,info.header.bits = 1,32
            bitmap = self.g.CreateDIBSection(desktop,ct.byref(info),0,ct.byref(bits),None,0)
            require(bool(bitmap) and bool(bits.value),'Capture DIB unavailable')
            previous = self.g.SelectObject(memory,bitmap)
            require(bool(previous) and previous != ct.c_void_p(-1).value,'Capture DIB selection failed')
            left,top = client['screen_left'],client['screen_top']
            if (width,height) == (client['width'],client['height']):
                copied = self.g.BitBlt(memory,0,0,width,height,desktop,left,top,0x40cc0020)
            else:
                require(self.g.SetStretchBltMode(memory,3) != 0,'Capture scaling mode failed')
                copied = self.g.StretchBlt(memory,0,0,width,height,desktop,left,top,client['width'],client['height'],0x40cc0020)
            require(copied and self.g.GdiFlush(),'Desktop capture copy failed')
            require(self.visible_client(pid,verify) == client,'Game client geometry changed during capture')
            png = desktop_png(ct.string_at(bits,width*height*4),width,height)
            return {'scope':'Visible Windows game client desktop pixels; not XR per-eye or a game D3D texture',
                    'client':client,'images':[{'eye':'window','width':width,'height':height,
                    'source_width':client['width'],'source_height':client['height'],
                    'png_base64':base64.b64encode(png).decode('ascii')}]}
        finally:
            if memory and previous and previous != ct.c_void_p(-1).value: self.g.SelectObject(memory,previous)
            if bitmap: self.g.DeleteObject(bitmap)
            if memory: self.g.DeleteDC(memory)
            if desktop: self.u.ReleaseDC(None,desktop)

    def send(self,key,up):
        packet = Input()
        packet.type = 1
        packet.ki.wScan = KEYS[key]
        packet.ki.dwFlags = 8 | (2 if up else 0) | (1 if key in ('UP','DOWN','LEFT','RIGHT') else 0)
        return self.u.SendInput(1,ct.byref(packet),ct.sizeof(Input)) == 1


def settle_input(windows,pid,verify,milliseconds,clock=None,pause=None):
    """Wait after helper preflight/focus, refusing any change of focused game window.

    No helper is launched during this interval. Identity and interactive-desktop
    checks belong to verify; focused_client checks the exact restored foreground
    HWND. Clock injection keeps offline deadline/focus-loss tests deterministic.
    """
    clock,pause = clock or time.monotonic,pause or time.sleep
    client = windows.focused_client(pid,verify)
    deadline = clock()+milliseconds/1000
    while True:
        verify()
        require(windows.focused_client(pid,verify)['hwnd'] == client['hwnd'],
                'Game foreground window changed during input settle')
        remaining = deadline-clock()
        if remaining <= 0:
            return
        pause(min(.02,remaining))


class Keyboard:
    def __init__(self,windows,pid,verify,settle_ms=1500):
        self.windows,self.pid,self.verify = windows,pid,verify
        self.settle_ms = settle_ms
        self.held = []

    def release(self):
        errors = []
        for key in list(reversed(self.held)):
            try:
                require(self.windows.send(key,True), 'KEYUP not queued')
                self.held.remove(key)
            except Exception:
                errors.append('KEYUP delivery failed for ' + key)
        return errors

    def hold(self,milliseconds,clock=time.monotonic,pause=time.sleep):
        deadline = clock() + milliseconds/1000
        while clock() < deadline:
            self.verify()
            require(self.windows.foreground_pid() == self.pid, 'Game lost foreground focus')
            pause(min(.02,max(0,deadline-clock())))

    def sequence(self,steps):
        require(not self.release() and not self.held,'Retained keyboard input could not be released')
        deliveries = 0
        try:
            self.verify()
            self.windows.focus(self.pid,self.verify)
            settle_input(self.windows,self.pid,self.verify,self.settle_ms)
            for step in steps:
                self.verify()
                self.windows.focused_client(self.pid,self.verify)
                for key in step['keys']:
                    self.verify()
                    require(self.windows.foreground_pid() == self.pid, 'Game lost foreground focus')
                    self.held.append(key)  # Release even after an ambiguous/failed SendInput result.
                    require(self.windows.send(key,False), 'KEYDOWN not queued')
                    deliveries += 1
                self.hold(step['duration_ms'])
                require(not self.release(), 'KEYUP delivery failed')
                if step.get('gap_ms',0):
                    self.hold(step['gap_ms'])
            return {'delivery_queued':True,'key_down_events':deliveries,
                    'scope':'Win32 SendInput queue delivery only; verify resulting game behavior with captures'}
        finally:
            failure = sys.exc_info()[1]
            errors = self.release()
            require(not errors, (str(failure)+'; ' if failure else '')+'; '.join(errors))


class Helper:
    def __init__(self,path): self.path = path

    def __call__(self,request):
        result = subprocess.run([str(self.path)],input=json.dumps(request),capture_output=True,
                                text=True,encoding='utf-8',timeout=10,check=False)
        response = json.loads(result.stdout)
        require(result.returncode == 0 and response.get('ok') is True,
                'Simulator helper failed: ' + str(response.get('error','nonzero exit')))
        return response['result']

    def driver(self,op,**arguments):
        result = self({'op':'driver_request','request':{'op':op,**arguments}})
        require(result.get('response',{}).get('ok') is True, 'Driver rejected request')
        return result


class Mouse:
    def __init__(self,windows,pid,verify,settle_ms=1500):
        self.windows,self.pid,self.verify = windows,pid,verify
        self.settle_ms = settle_ms
        self.held = False

    def release(self):
        if self.held:
            require(self.windows.mouse_button(True),'Mouse LEFTUP not queued')
            self.held = False

    def click(self,x,y,duration):
        self.release()
        require(not self.held,'Retained mouse input could not be released')
        self.windows.focus(self.pid,self.verify)
        settle_input(self.windows,self.pid,self.verify,self.settle_ms)
        client = self.windows.focused_client(self.pid,self.verify)
        dx,dy = client_point(x,y,client['width'],client['height'])
        screen = (client['screen_left']+dx,client['screen_top']+dy)
        require(self.windows.point_visible(screen),'Mouse point is outside the visible desktop')
        before = self.windows.cursor()
        require(self.windows.u.SetCursorPos(*screen),'Cursor movement failed')
        try:
            for _ in range(2):  # Recheck the actual point immediately before LEFTDOWN.
                self.verify()
                require(self.windows.focused_client(self.pid,self.verify) == client,'Game client moved before click')
                require(self.windows.point_visible(screen),'Mouse point left the visible desktop')
                require(self.windows.cursor() == screen,'Cursor did not reach requested client point')
                hit = self.windows.u.WindowFromPoint(Point(*screen))
                require(self.windows.window_pid(hit) == self.pid and
                        (hit == client['hwnd'] or self.windows.u.IsChild(client['hwnd'],hit)),
                        'Requested point is not the primary game client or its child')
            self.held = True
            require(self.windows.mouse_button(False),'Mouse LEFTDOWN not queued')
            deadline = time.monotonic()+duration/1000
            while time.monotonic() < deadline:
                self.verify()
                require(self.windows.foreground_pid() == self.pid,'Game lost foreground during click')
                time.sleep(min(.01,max(0,deadline-time.monotonic())))
            self.verify()
            require(self.windows.foreground_pid() == self.pid,'Game lost foreground during click')
            return {'delivery_queued':True,'client':client,'client_pixel':[dx,dy],
                    'screen_pixel':list(screen),'cursor_before':list(before),
                    'scope':'Win32 left-button queue delivery only; verify Launch behavior using captures'}
        finally:
            failure = sys.exc_info()[1]
            try: self.release()
            except Exception as error:
                raise ValueError((str(failure)+'; ' if failure else '')+str(error)) from error


class Host:
    def __init__(self,windows,helper,identity,input_settle_ms=1500):
        self.windows,self.helper,self.identity = windows,helper,identity
        self.activated = False
        self.completion_unknown_count = 0
        self.keyboard = Keyboard(windows,identity['pid'],self.verify,input_settle_ms)
        self.mouse = Mouse(windows,identity['pid'],self.verify,input_settle_ms)

    def verify(self):
        require(self.windows.identity(self.identity['pid']) == self.identity, 'Game exited or PID identity changed')
        self.windows.interactive()

    def runtime(self,input_required=False):
        self.verify()
        runtime = self.helper({'op':'status'})
        require(runtime.get('scene_focus_process') == self.identity['pid'], 'Runtime scene PID differs from target')
        heads = [d for d in runtime['devices'] if d.get('device_class') == 1 and d.get('connected') is True]
        require(len(heads) == 1 and heads[0].get('index') == 0 and heads[0].get('serial') == 'modlinker_sim_head',
                'Foreign or ambiguous HMD selection')
        require(runtime.get('chaperone',{}).get('ready') is True, 'Chaperone not ready')
        poses = self.helper({'op':'poses','origin':'standing'})
        head = [p for p in poses['devices'] if p['device'].get('index') == 0]
        require(len(head) == 1 and head[0].get('valid') is True and head[0].get('connected') is True
                and head[0].get('tracking_result') == 200, 'Runtime HMD not tracked Running_OK')
        driver = self.helper.driver('status')
        require(driver['response']['status'].get('mode') == 'virtual_hmd', 'Driver not in virtual_hmd mode')
        if input_required:
            dashboard = runtime.get('dashboard',{})
            require(dashboard.get('known') is True and dashboard.get('visible') is False,
                    'Dashboard visible or unknown; input refused')
        return {'runtime':runtime,'poses':poses,'driver':driver}

    def stop_driver(self):
        owned = self.activated
        errors = self.keyboard.release()
        try: self.mouse.release()
        except Exception as error: errors.append(str(error))
        if self.activated:
            for op in ('cancel','stop'):
                try:
                    result = self.helper.driver(op)
                    if op == 'stop':
                        require(result['response']['status'].get('active') is False, 'Driver remained active')
                        self.activated = False
                except Exception as error:
                    errors.append(op + ': ' + str(error))
        require(not errors,'; '.join(errors))
        return {'stopped':True,'owned':owned}

    def input_interlock(self):
        """Retry every retained modality; refuse new downs until all releases queue."""
        errors = self.keyboard.release()
        try: self.mouse.release()
        except Exception as error: errors.append(str(error))
        require(not errors and not self.keyboard.held and not self.mouse.held,
                'Retained input cleanup failed; new input refused: '+'; '.join(errors))

    def dispatch(self,name,args):
        require(type(name) is str and name in TOOLS,'Unsupported tool')
        if name == 'get_state':
            fields(args,())
            current = self.windows.identity(self.identity['pid'])
            return {'alive':current == self.identity,'target':self.identity,'observed_process':current,
                    'foreground_pid':self.windows.foreground_pid(),'driver_owned':self.activated,
                    'held_keys':list(self.keyboard.held),'mouse_held':self.mouse.held,
                    'completion_unknown_count':self.completion_unknown_count,
                    'input_settle_ms':self.keyboard.settle_ms,
                    'windows':self.windows.describe(self.identity['pid'])}
        if name == 'mouse_click':
            fields(args,('x','y','duration_ms'),('x','y'))
            client_point(args['x'],args['y'],1,1)  # Validate before focus or cursor changes.
            duration = integer(args.get('duration_ms',50),1,500)
            self.input_interlock()
            self.runtime(True)
            return self.mouse.click(args['x'],args['y'],duration)
        if name == 'capture_window':
            fields(args,('max_edge',))
            edge = integer(args.get('max_edge',1280),512,2048)
            self.runtime()
            return self.windows.capture(self.identity['pid'],edge,self.verify)
        if name in ('keyboard_input','keyboard_sequence'):
            steps = key_steps(args,name == 'keyboard_sequence')
            self.input_interlock()
            self.runtime(True)
            return self.keyboard.sequence(steps)
        if name in ('xr_sim_status','xr_sim_start','xr_sim_stop'):
            fields(args,())
            if name == 'xr_sim_stop':
                self.verify()
                return self.stop_driver()
            observed = self.runtime(name == 'xr_sim_start')
            if name == 'xr_sim_status':
                return observed
            require(not self.activated and observed['driver']['response']['status'].get('active') is False,
                    'Simulator already active; refusing foreign control lease')
            self.activated = True
            return self.helper.driver('start',left={'tracked':False},right={'tracked':False})
        if name == 'xr_sim_capture':
            fields(args,('eye','max_edge'))
            eye,max_edge = args.get('eye','both'),args.get('max_edge',768)
            require(eye in ('left','right','both'),'Unknown eye')
            integer(max_edge,256,768 if eye == 'both' else 1024)
            self.runtime()
            return self.helper({'op':'capture','eye':eye,'max_edge':max_edge})  # Forward actual PNGs for native MCP image lifting.
        if name == 'xr_sim_idle':
            action = idle_arguments(args)
            require(self.activated,'Start this host simulation before changing idle animation')
            if action == 'stop':
                # Owned cancellation remains available after foreground/dashboard loss.
                self.verify()
                return self.helper.driver('idle',**args)
            observed = self.runtime(True)
            support = observed['driver']['response']['status'].get('idle_support',{})
            require(support.get('version') == 'natural-idle-v1' and
                    support.get('max_duration_ms') == 60000 and support.get('lease_timeout_ms') == 5000,
                    'Driver does not support the bounded natural-idle-v1 contract')
            self.verify()
            require(self.windows.foreground_pid() == self.identity['pid'],
                    'Game lost foreground focus; idle start/renew refused')
            return self.helper.driver('idle',**args)
        require(self.activated,'Start this host simulation before changing poses/eyes')
        if name in ('xr_sim_set_pose','xr_sim_set_eyes'):
            smooth_duration(args)
        self.runtime(True)
        if name == 'xr_sim_set_pose':
            pose(args,('device','smooth_seconds'))
            require('device' in args,'device required')
            return self.helper.driver('update',**args)
        if name == 'xr_sim_set_eyes':
            fields(args,('active','left','right','smooth_seconds'),('active',))
            require(type(args['active']) is bool,'active must be boolean')
            for eye in ('left','right'):
                if eye in args: pose(args[eye])
            return self.helper.driver('eyes',**args)
        fields(args,('duration_ms','steps'),('duration_ms','steps'))
        duration = integer(args['duration_ms'],1,60000)
        require(type(args['steps']) is list and 1 <= len(args['steps']) <= 64,'Expected 1..64 replay steps')
        previous = 0
        for step in args['steps']:
            pose(step,('device','at_ms'))
            require('device' in step and 'at_ms' in step,'Replay device/time required')
            previous = integer(step['at_ms'],previous,duration)
        return self.helper.driver('replay',**args)


class Transport:
    """Cdecl ABI from Mod Linker addons/mcp/native/src/ffi.rs; no Unity host dependency."""
    def __init__(self,path):
        self.dll = ct.CDLL(str(path),winmode=0x1100)
        self.started = False
        for name in ('next','event','status'):
            fn = getattr(self.dll,'modlinker_mcp_'+name)
            fn.argtypes,fn.restype = [ct.c_void_p,ct.c_size_t,ct.POINTER(ct.c_size_t)],ct.c_int32
        self.dll.modlinker_mcp_start.argtypes,self.dll.modlinker_mcp_start.restype = [ct.c_char_p,ct.c_size_t],ct.c_int32
        self.dll.modlinker_mcp_complete.argtypes,self.dll.modlinker_mcp_complete.restype = [ct.c_uint64,ct.c_char_p,ct.c_size_t],ct.c_int32
        self.dll.modlinker_mcp_stop.argtypes,self.dll.modlinker_mcp_stop.restype = [],None

    def start(self):
        version = b'edup-vr-host-1'
        require(self.dll.modlinker_mcp_start(version,len(version)) == 0,'MCP transport start failed')
        self.started = True
        status = self.read('status')
        require(status.get('bindAddress') == '127.0.0.1' and status.get('configuredUrl') == 'http://127.0.0.1:17832/mcp',
                'Only the supplied loopback transport is permitted')
        return status

    def read(self,name):
        size = 65536
        for _ in range(2):
            buffer,required = ct.create_string_buffer(size),ct.c_size_t()
            code = getattr(self.dll,'modlinker_mcp_'+name)(buffer,size,ct.byref(required))
            if code == 0:
                return None
            if code == -2:
                require(size < required.value <= 262144,'MCP job exceeds permitted buffer')
                size = required.value
                continue
            require(code == 1 and required.value <= size,'MCP read failed')
            return json.loads(buffer.raw[:required.value])
        raise ValueError('MCP read retry exceeded')

    def complete(self,job,payload):
        data = json.dumps(payload,allow_nan=False).encode('utf-8')
        code = self.dll.modlinker_mcp_complete(job,data,len(data))
        # The native pending request can expire while an already executing tool finishes.
        # Effects must never be retried merely because its reply no longer has a receiver.
        require(code in (0,-3),'MCP completion failed: '+str(code))
        return 'delivered' if code == 0 else 'unknown'

    def stop(self):
        if self.started:
            self.started = False
            self.dll.modlinker_mcp_stop()


def serve(transport,host,stop_file,events):
    while stop_file is None or not stop_file.exists():
        for _ in range(32):
            if transport.read('event') is None:
                break  # Deliberately discard native metadata rather than retain client identifiers/arguments.
        job = transport.read('next')
        if job is None:
            time.sleep(.01)
            continue
        success = False
        name = job.get('name')
        try:
            data = host.dispatch(name,job.get('arguments',{}))
            payload,success = {'name':name,'data':data},True
        except Exception as error:
            payload = {'error':str(error)}
        completion = transport.complete(integer(job['id'],0,2**64-1),payload)
        if completion == 'unknown':
            host.completion_unknown_count += 1
        events.append({'tool':name if type(name) is str and name in TOOLS else 'unsupported','ok':success,
                       'completion':completion,
                       'utc':datetime.now(timezone.utc).isoformat()})


def save(path,data):
    with path.open('x',encoding='utf-8') as stream:
        json.dump(data,stream,indent=2,allow_nan=False)
        stream.write('\n')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('transport-dll','client','game-dir','output','stop-file'):
        parser.add_argument('--'+name,type=Path)
    parser.add_argument('--expect-scene-pid',type=int)
    parser.add_argument('--session-id',type=int,help='Optional expected interactive session; defaults to current session')
    parser.add_argument('--input-settle-ms',type=int,default=1500,
                        help='Guarded focus settle after runtime preflight, once per input request (0..2000 ms)')
    parser.add_argument('--dry-run',action='store_true')
    parser.add_argument('--self-test',action='store_true')
    args = parser.parse_args(argv)
    if args.self_test:
        return 0 if unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(Tests)).wasSuccessful() else 1
    try:
        for path in (args.transport_dll,args.client):
            require(path is not None and path.is_absolute() and path.is_file(),'Explicit absolute existing DLL/client required')
        require(args.game_dir is not None and args.game_dir.is_absolute() and
                (args.game_dir/'EliteDangerous64.exe').is_file(),'Absolute Elite product folder required')
        integer(args.expect_scene_pid,1,2**32-1)
        integer(args.input_settle_ms,0,2000)
        if args.session_id is not None:
            integer(args.session_id,1,2**32-1)
        require(args.output is not None and args.output.is_absolute() and args.output.parent.is_dir()
                and not args.output.exists() and not args.output.is_symlink(),'Absolute output must be new')
        require(args.stop_file is None or args.stop_file.is_absolute(),'Stop file must be absolute')
        require(args.stop_file is None or not args.stop_file.exists(),'Stop file already exists')
        if args.dry_run:
            print('Would host loopback MCP for the explicit game identity; no DLL/helper/input/files touched')
            return 0
        windows = Windows()
        current_session = windows.environment()
        require(args.session_id is None or current_session == args.session_id,'Host is in a different interactive session')
        identity = windows.identity(args.expect_scene_pid)
        require(identity is not None and identity['session'] == current_session and
                os.path.normcase(identity['image']) == os.path.normcase(str(args.game_dir/'EliteDangerous64.exe')),
                'PID is not the expected Elite image in the host interactive session')
        host = Host(windows,Helper(args.client),identity,args.input_settle_ms)
        observed = host.runtime()
        require(observed['driver']['response']['status'].get('active') is False,'Existing simulator control lease refused')
        args.output.mkdir()
        events,cleanup_errors = deque(maxlen=128),[]
        transport,failed = None,False
        try:
            transport = Transport(args.transport_dll)
            status = transport.start()
            save(args.output/'readiness.json',{'host_pid':os.getpid(),'target':identity,'transport':status,
                 'helper_sha256':hashlib.sha256(args.client.read_bytes()).hexdigest(),
                 'transport_sha256':hashlib.sha256(args.transport_dll.read_bytes()).hexdigest(),
                 'endpoint':'http://127.0.0.1:17832/mcp','initial_runtime':observed,
                 'input_settle_ms':args.input_settle_ms,
                 'scope':'External queued tools; no game injection; SendInput acceptance is not gameplay proof'})
            print('http://127.0.0.1:17832/mcp',flush=True)
            serve(transport,host,args.stop_file,events)
        except KeyboardInterrupt:
            pass
        except Exception as error:
            failed = True
            cleanup_errors.append('Host: '+str(error))
        finally:
            try: host.stop_driver()
            except Exception as error: cleanup_errors.append('Input cleanup: '+str(error))
            if transport is not None:
                try: transport.stop()
                except Exception as error: cleanup_errors.append('Transport cleanup: '+str(error))
            save(args.output/'shutdown.json',{'stopped':True,'errors':cleanup_errors,'events':list(events),
                                           'driver_owned_after_cleanup':host.activated,'held_keys':host.keyboard.held,
                                           'mouse_held':host.mouse.held})
        return 1 if failed or cleanup_errors else 0
    except (OSError,ValueError) as error:
        print('edup_vr_mcp: '+str(error),file=sys.stderr)
        return 2


class Tests(unittest.TestCase):
    def test_describe_reports_same_context_virtual_desktop(self):
        windows = Windows.__new__(Windows)
        windows.u = mock.Mock()
        windows.u.GetForegroundWindow.return_value = 100
        windows.u.GetSystemMetrics.side_effect = lambda index:{76:-1920,77:0,78:3840,79:1080}[index]
        windows.client = mock.Mock(return_value={'hwnd':100,'width':1280,'height':720,'screen_left':0,'screen_top':0})
        observed = windows.describe(42)
        self.assertEqual(observed['virtual_desktop'],[-1920,0,3840,1080])
        self.assertEqual(observed['target_client']['width'],1280)
        windows.u.SetForegroundWindow.assert_not_called()

    def test_focus_restores_iconic_main_window_before_same_pid_return(self):
        for iconic,foreground in ((True,100),(False,101)):
            with self.subTest(iconic=iconic,foreground=foreground):
                windows = Windows.__new__(Windows)
                windows.u,windows.k = mock.Mock(),mock.Mock()
                windows.top_windows = mock.Mock(return_value=[200,101,100])
                windows.window_pid = lambda hwnd: 77 if hwnd == 200 else 42
                state = {'iconic':iconic,'foreground':foreground}
                windows.u.IsWindowVisible.return_value = 1
                windows.u.IsIconic.side_effect = lambda hwnd: state['iconic'] if hwnd == 100 else False
                windows.u.GetForegroundWindow.side_effect = lambda: state['foreground']
                def rect(hwnd,pointer):
                    value = ct.cast(pointer,ct.POINTER(Rect)).contents
                    value.right,value.bottom = (1920,1080) if hwnd == 100 else (320,200)
                    return 1
                windows.u.GetClientRect.side_effect = rect
                def class_name(hwnd,buffer,capacity):
                    buffer.value = 'FrontierDevelopmentsAppWinClass' if hwnd == 100 else 'auxiliary'
                    return len(buffer.value)
                windows.u.GetClassNameW.side_effect = class_name
                def restore(hwnd,command):
                    self.assertEqual((hwnd,command),(100,9))
                    state['iconic'] = False
                    return 1
                windows.u.ShowWindow.side_effect = restore
                windows.u.SetForegroundWindow.side_effect = lambda hwnd: state.update(foreground=hwnd) or 1
                verify = mock.Mock()
                with mock.patch.object(time,'sleep'):
                    windows.focus(42,verify)
                if iconic:
                    windows.u.ShowWindow.assert_called_with(100,9)
                self.assertFalse(state['iconic'])
                self.assertEqual(state['foreground'],100)
                self.assertTrue(verify.called)
                self.assertTrue(all(call.args[0] == 100 for call in windows.u.ShowWindow.call_args_list))

    def fixture(self):
        identity = {'pid':42,'creation_ticks':'100','image':'game','session':7}
        windows = mock.Mock()
        windows.identity.return_value,windows.foreground_pid.return_value = identity,42
        windows.describe.return_value = {}
        windows.focused_client.return_value = {'hwnd':100}
        windows.send.return_value = True
        helper = mock.Mock()
        def call(request):
            if request['op'] == 'status':
                return {'scene_focus_process':42,'devices':[{'device_class':1,'connected':True,'index':0,'serial':'modlinker_sim_head'}],
                        'chaperone':{'ready':True},'dashboard':{'known':True,'visible':False}}
            if request['op'] == 'poses':
                return {'devices':[{'device':{'index':0},'valid':True,'connected':True,'tracking_result':200}]}
            return {'images':[{'eye':'left','png_base64':'real-png'}]}
        helper.side_effect = call
        helper.driver.return_value = {'response':{'ok':True,'status':{'mode':'virtual_hmd','active':False}}}
        return Host(windows,helper,identity,0),windows,helper

    def test_input_settle_deadline_and_runtime_order(self):
        host,windows,helper = self.fixture()
        host.keyboard.settle_ms = 1500
        ticks = [0.0]
        downs = []
        def pause(seconds):
            self.assertFalse(downs)
            self.assertEqual(len(helper.call_args_list),2)  # Status and poses precede settling.
            ticks[0] += seconds
        windows.send.side_effect = lambda key,up: downs.append(ticks[0]) is None if not up else True
        original = settle_input
        with mock.patch.object(sys.modules[__name__],'settle_input',side_effect=lambda w,p,v,m:
                               original(w,p,v,m,lambda:ticks[0],pause)), \
             mock.patch.object(host.keyboard,'hold'):
            host.dispatch('keyboard_sequence',{'steps':[{'keys':['ENTER'],'duration_ms':1},
                                                       {'keys':['RIGHT'],'duration_ms':1}]})
        self.assertEqual(downs,[1.5,1.5])  # Once per request, not another settle for each step.
        self.assertGreater(windows.focused_client.call_count,50)
        self.assertEqual(host.keyboard.held,[])

    def test_settle_focus_loss_refuses_down_and_can_recover(self):
        host,windows,_ = self.fixture()
        host.keyboard.settle_ms = 1500
        ticks,fail = [0.0],[True]
        def client(pid,verify):
            verify()
            require(not (fail[0] and ticks[0] >= .5),'Game lost foreground focus')
            return {'hwnd':100}
        windows.focused_client.side_effect = client
        def pause(seconds): ticks[0] += seconds
        original = settle_input
        with mock.patch.object(sys.modules[__name__],'settle_input',side_effect=lambda w,p,v,m:
                               original(w,p,v,m,lambda:ticks[0],pause)), \
             mock.patch.object(host.keyboard,'hold'):
            with self.assertRaisesRegex(ValueError,'foreground'):
                host.dispatch('keyboard_input',{'keys':['ENTER'],'duration_ms':1})
            windows.send.assert_not_called()
            self.assertEqual(host.keyboard.held,[])
            fail[0] = False
            host.dispatch('keyboard_input',{'keys':['ENTER'],'duration_ms':1})
        self.assertGreaterEqual(ticks[0],2)
        self.assertEqual(windows.send.call_args_list,[mock.call('ENTER',False),mock.call('ENTER',True)])

    def test_mouse_settle_precedes_geometry_cursor_and_button(self):
        windows = mock.Mock()
        ticks,fail = [0.0],[False]
        client = {'hwnd':100,'width':101,'height':81,'screen_left':10,'screen_top':20}
        def focused(pid,verify):
            verify()
            require(not (fail[0] and ticks[0] >= .5),'Game lost foreground focus')
            return client
        windows.focused_client.side_effect = focused
        windows.cursor.side_effect = [(9,9),(60,60),(60,60)]
        windows.window_pid.return_value = windows.foreground_pid.return_value = 42
        windows.u.WindowFromPoint.return_value = 100
        downs = []
        windows.u.SetCursorPos.side_effect = lambda *args: self.assertGreaterEqual(ticks[0],1.5) is None
        windows.mouse_button.side_effect = lambda up: downs.append(ticks[0]) is None if not up else True
        def pause(seconds): ticks[0] += seconds
        mouse = Mouse(windows,42,mock.Mock(),1500)
        with mock.patch.object(time,'monotonic',side_effect=lambda:ticks[0]), \
             mock.patch.object(time,'sleep',side_effect=pause):
            mouse.click(.5,.5,1)
            self.assertEqual(downs,[1.5])
            self.assertFalse(mouse.held)
            ticks[0],fail[0] = 0,True
            windows.u.SetCursorPos.reset_mock()
            with self.assertRaisesRegex(ValueError,'foreground'):
                mouse.click(.5,.5,1)
            windows.u.SetCursorPos.assert_not_called()
            self.assertEqual(downs,[1.5])

    def test_abi_and_keys(self):
        self.assertEqual((ct.sizeof(Input),Input.data.offset),(40,8))
        for key in ('ALT','WIN','CTRL','F4',';shutdown','A B'):
            with self.assertRaises(ValueError): key_steps({'keys':[key],'duration_ms':1})
        with self.assertRaises(ValueError): key_steps({'steps':[{'keys':['W'],'duration_ms':2000}]*6},True)
        with self.assertRaises(ValueError): key_steps({'keys':['A','A'],'duration_ms':1})
        self.assertEqual((KEYS['SHIFT'],KEYS['BACKSPACE']),(0x2a,0x0e))
        self.assertEqual(key_steps({'keys':['SHIFT','BACKSPACE'],'duration_ms':1})[0]['keys'],['SHIFT','BACKSPACE'])

    def test_identity_and_focus_cleanup(self):
        host,windows,_ = self.fixture()
        windows.identity.return_value = {**host.identity,'creation_ticks':'101'}
        with self.assertRaises(ValueError): host.verify()
        self.assertFalse(host.dispatch('get_state',{})['alive'])
        windows.identity.return_value = host.identity
        windows.foreground_pid.side_effect = [42,77]
        with self.assertRaises(ValueError): host.keyboard.sequence([{'keys':['W'],'duration_ms':50}])
        self.assertEqual(windows.send.call_args_list,[mock.call('W',False),mock.call('W',True)])
        self.assertEqual(host.keyboard.held,[])

    def test_dispatch_and_capture(self):
        host,windows,helper = self.fixture()
        with mock.patch.object(sys.modules[__name__],'settle_input',side_effect=AssertionError('Read-only tool settled')):
            self.assertTrue(host.dispatch('get_state',{})['alive'])
            host.dispatch('xr_sim_status',{})
            host.dispatch('capture_window',{})
            self.assertEqual(host.dispatch('xr_sim_capture',{'eye':'left','max_edge':1024})['images'][0]['png_base64'],'real-png')
        for name in ('eval','shell','compile_csharp','launch_game'):
            with self.assertRaises(ValueError): host.dispatch(name,{})
        with self.assertRaises(ValueError): host.dispatch('xr_sim_capture',{'output_directory':'C:/foreign'})
        host.dispatch('xr_sim_start',{})
        self.assertTrue(host.activated)
        helper.driver.assert_called_with('start',left={'tracked':False},right={'tracked':False})
        helper.driver.side_effect = ValueError('actual driver rejection')
        with self.assertRaisesRegex(ValueError,'actual driver rejection'):
            host.dispatch('xr_sim_set_pose',{'device':'head','position':[0,1.6,0]})
        helper.driver.side_effect = None
        host.stop_driver()
        self.assertFalse(host.activated)
        self.assertEqual([c.args[0] for c in helper.driver.call_args_list][-2:],['cancel','stop'])

    def test_existing_driver_smooth_duration_bounds_and_cleanup(self):
        host,_,helper = self.fixture()
        host.dispatch('xr_sim_start',{})
        for name,arguments,operation in (
                ('xr_sim_set_pose',{'device':'head','orientation':[0,0,0,1]},'update'),
                ('xr_sim_set_eyes',{'active':True,'left':{'tracked':True}},'eyes')):
            for seconds in (0,.25,60):
                with self.subTest(tool=name,seconds=seconds):
                    host.dispatch(name,{**arguments,'smooth_seconds':seconds})
                    helper.driver.assert_called_with(operation,**arguments,smooth_seconds=seconds)
            for seconds in (-.001,60.001,float('nan'),float('inf'),-float('inf'),True,'1',None):
                with self.subTest(tool=name,rejected=seconds):
                    helper.driver.reset_mock()
                    with self.assertRaisesRegex(ValueError,'smooth_seconds'):
                        host.dispatch(name,{**arguments,'smooth_seconds':seconds})
                    helper.driver.assert_not_called()
        # Driver conflicts remain visible; stopping still cancels that host's motion.
        helper.driver.side_effect = ValueError('current smooth motion')
        with self.assertRaisesRegex(ValueError,'current smooth motion'):
            host.dispatch('xr_sim_set_pose',{'device':'head','smooth_seconds':.5})
        helper.driver.side_effect = None
        host.stop_driver()
        self.assertFalse(host.activated)
        self.assertEqual([c.args[0] for c in helper.driver.call_args_list][-2:],['cancel','stop'])

    def test_idle_schema_bounds_are_validated_before_runtime_effects(self):
        host,_,helper = self.fixture()
        host.activated = True
        for arguments in ({}, {'action':'START'}, {'action':'start'},
                {'action':'start','duration_ms':60000}, {'action':'start','seed':17},
                {'action':'renew','seed':17}, {'action':'stop','duration_ms':1},
                {'action':'start','duration_ms':1,'seed':0,'unexpected':True}):
            with self.subTest(arguments=arguments):
                helper.reset_mock()
                with self.assertRaises(ValueError): host.dispatch('xr_sim_idle',arguments)
                helper.assert_not_called()
                helper.driver.assert_not_called()
        for field,values in (('duration_ms',(0,60001,-1,True,None,'1',1.0,float('nan'),float('inf'))),
                ('seed',(-1,2**32,True,None,'1',1.5,float('nan'),float('inf')))):
            for value in values:
                helper.reset_mock()
                with self.assertRaises(ValueError):
                    host.dispatch('xr_sim_idle',{'action':'start','duration_ms':60000,'seed':17,field:value})
                helper.assert_not_called()
                helper.driver.assert_not_called()

    def test_idle_requires_own_lease_supported_driver_and_foreground(self):
        host,windows,helper = self.fixture()
        start = {'action':'start','duration_ms':60000,'seed':17}
        helper.reset_mock()
        with self.assertRaisesRegex(ValueError,'Start this host'): host.dispatch('xr_sim_idle',start)
        helper.assert_not_called()
        host.activated = True
        observed = {'driver':{'response':{'status':{'idle_support':{
            'version':'natural-idle-v1','max_duration_ms':60000,'lease_timeout_ms':5000}}}}}
        with mock.patch.object(host,'runtime',return_value=observed) as runtime:
            windows.foreground_pid.return_value = host.identity['pid']
            for duration in (1,60000):
                for seed in (0,2**32-1):
                    host.dispatch('xr_sim_idle',{'action':'start','duration_ms':duration,'seed':seed})
                    helper.driver.assert_called_with('idle',action='start',duration_ms=duration,seed=seed)
            host.dispatch('xr_sim_idle',{'action':'renew'})
            helper.driver.assert_called_with('idle',action='renew')
            runtime.assert_called_with(True)
            helper.driver.reset_mock()
            windows.foreground_pid.return_value = 999
            with self.assertRaisesRegex(ValueError,'foreground'): host.dispatch('xr_sim_idle',start)
            helper.driver.assert_not_called()
            windows.foreground_pid.return_value = host.identity['pid']
            for support in ({},{'version':'old'}, {'version':'natural-idle-v1','max_duration_ms':60001,'lease_timeout_ms':5000}):
                observed['driver']['response']['status']['idle_support'] = support
                with self.assertRaisesRegex(ValueError,'does not support'): host.dispatch('xr_sim_idle',start)
                helper.driver.assert_not_called()

    def test_idle_stop_remains_available_after_focus_or_dashboard_loss(self):
        host,windows,helper = self.fixture()
        host.activated = True
        windows.foreground_pid.return_value = 999
        with mock.patch.object(host,'runtime',side_effect=AssertionError('stop must bypass active-input preflight')):
            host.dispatch('xr_sim_idle',{'action':'stop'})
            helper.driver.assert_called_with('idle',action='stop')
        host.stop_driver()
        self.assertEqual([call.args[0] for call in helper.driver.call_args_list][-2:],['cancel','stop'])
        self.assertFalse(host.activated)

    def test_shift_backspace_scancode_delivery(self):
        windows = Windows.__new__(Windows)
        windows.u = mock.Mock()
        packets = []
        def send(count,pointer,size):
            packet = ct.cast(pointer,ct.POINTER(Input)).contents
            packets.append((count,size,packet.type,packet.ki.wVk,packet.ki.wScan,packet.ki.dwFlags))
            return 1
        windows.u.SendInput.side_effect = send
        for key in ('SHIFT','BACKSPACE','COMMA','PERIOD'):
            self.assertTrue(windows.send(key,False))
            self.assertTrue(windows.send(key,True))
        self.assertEqual(packets,[(1,40,1,0,0x2a,8),(1,40,1,0,0x2a,10),
                                 (1,40,1,0,0x0e,8),(1,40,1,0,0x0e,10),
                                 (1,40,1,0,0x33,8),(1,40,1,0,0x33,10),
                                 (1,40,1,0,0x34,8),(1,40,1,0,0x34,10)])

    def test_retained_shift_blocks_enter_and_mouse_until_all_releases_succeed(self):
        host,windows,_ = self.fixture()
        windows.send.side_effect = lambda key,up: not (key == 'SHIFT' and up)
        host.keyboard.hold = mock.Mock()
        with self.assertRaises(ValueError): host.keyboard.sequence([{'keys':['SHIFT'],'duration_ms':1}])
        self.assertEqual(host.keyboard.held,['SHIFT'])
        windows.send.reset_mock()
        with self.assertRaises(ValueError): host.keyboard.sequence([{'keys':['ENTER'],'duration_ms':1}])
        self.assertNotIn(mock.call('ENTER',False),windows.send.call_args_list)
        host.mouse.held = True
        windows.mouse_button.return_value = False
        for tool,args in (('keyboard_input',{'keys':['ENTER'],'duration_ms':1}),
                          ('mouse_click',{'x':.5,'y':.5})):
            windows.send.reset_mock()
            windows.mouse_button.reset_mock()
            with self.assertRaisesRegex(ValueError,'Retained input cleanup'):
                host.dispatch(tool,args)
            windows.send.assert_called_with('SHIFT',True)
            windows.mouse_button.assert_called_once_with(True)
            self.assertNotIn(mock.call('ENTER',False),windows.send.call_args_list)
            self.assertNotIn(mock.call(False),windows.mouse_button.call_args_list)
        windows.send.side_effect = None
        windows.send.return_value = windows.mouse_button.return_value = True
        result = host.dispatch('keyboard_input',{'keys':['ENTER'],'duration_ms':1})
        self.assertTrue(result['delivery_queued'])
        self.assertEqual(host.keyboard.held,[])
        self.assertFalse(host.mouse.held)

    def test_mouse_packets_and_focus_loss(self):
        windows = Windows.__new__(Windows)
        windows.u = mock.Mock()
        packets = []
        def send(count,pointer,size):
            packet = ct.cast(pointer,ct.POINTER(Input)).contents
            packets.append((count,size,packet.type,packet.mi.dx,packet.mi.dy,packet.mi.dwFlags))
            return 1
        windows.u.SendInput.side_effect = send
        self.assertTrue(windows.mouse_button(False))
        self.assertTrue(windows.mouse_button(True))
        self.assertEqual(packets,[(1,40,0,0,0,2),(1,40,0,0,0,4)])
        windows = mock.Mock()
        windows.focused_client.return_value = {'hwnd':100,'width':101,'height':81,'screen_left':10,'screen_top':20}
        windows.cursor.side_effect = [(9,9),(60,60),(60,60)]
        windows.window_pid.return_value = 42
        windows.foreground_pid.return_value = 77
        windows.mouse_button.return_value = True
        mouse = Mouse(windows,42,mock.Mock(),0)
        with self.assertRaisesRegex(ValueError,'foreground'):
            mouse.click(.5,.5,50)
        windows.u.SetCursorPos.assert_called_once_with(60,60)
        self.assertEqual(windows.mouse_button.call_args_list,[mock.call(False),mock.call(True)])
        self.assertFalse(mouse.held)

    def test_pointer_bounds_and_capture_layout(self):
        self.assertEqual(client_point(0,1,1920,1080),(0,1079))
        self.assertEqual(capture_size(3840,2160,1280),(1280,720))
        for x,y in ((-1,0),(0,1.1),(True,.5),(float('nan'),0)):
            host,windows,_ = self.fixture()
            with self.assertRaises(ValueError): host.dispatch('mouse_click',{'x':x,'y':y})
            windows.focus.assert_not_called()
        self.assertEqual(ct.sizeof(BitmapHeader),40)
        with self.assertRaisesRegex(ValueError,'RGB-black'):
            desktop_png(b'\0\0\0\xff',1,1)
        png = desktop_png(b'\x03\x02\x01\0',1,1)
        self.assertEqual(struct.unpack('>II',png[16:24]),(1,1))
        size = struct.unpack('>I',png[33:37])[0]
        self.assertEqual(zlib.decompress(png[41:41+size]),b'\0\x01\x02\x03\xff')

    def test_mouse_uses_point_ownership_not_unrelated_overlap(self):
        for hit,hit_pid,child,allowed in ((100,42,False,True),(102,42,True,True),
                                          (200,77,False,False),(101,42,False,False)):
            with self.subTest(hit=hit):
                windows = Windows.__new__(Windows)
                windows.u = mock.Mock()
                windows.focus = mock.Mock()
                windows.client = mock.Mock(return_value={'hwnd':100,'width':101,'height':81,'screen_left':10,'screen_top':20})
                windows.window_pid = lambda hwnd: 42 if hwnd == 100 else hit_pid
                windows.u.GetForegroundWindow.return_value = 100
                windows.u.IsIconic.return_value = False
                windows.u.GetSystemMetrics.side_effect = lambda index:{76:0,77:0,78:100,79:100}[index]
                windows.u.WindowFromPoint.return_value = hit
                windows.u.IsChild.return_value = int(child)
                windows.cursor = mock.Mock(side_effect=[(9,9),(60,60),(60,60)])
                windows.mouse_button = mock.Mock(return_value=True)
                # A foreign 1x1 thumbnail elsewhere must not invoke the full-frame capture gate.
                windows.top_windows = mock.Mock(side_effect=AssertionError('Mouse must not use full-client overlap guard'))
                mouse = Mouse(windows,42,mock.Mock(),0)
                with mock.patch.object(time,'monotonic',side_effect=[0,0,0,1]):
                    if allowed:
                        self.assertTrue(mouse.click(.5,.5,1)['delivery_queued'])
                    else:
                        with self.assertRaisesRegex(ValueError,'primary game client'):
                            mouse.click(.5,.5,1)
                self.assertEqual(windows.mouse_button.call_args_list,
                                 [mock.call(False),mock.call(True)] if allowed else [])
                windows.top_windows.assert_not_called()
                self.assertFalse(mouse.held)
                with self.assertRaisesRegex(ValueError,'extends outside'):
                    windows.visible_client(42,mock.Mock())
                windows.mouse_button.reset_mock()
                with self.assertRaisesRegex(ValueError,'outside the visible desktop'):
                    mouse.click(1,1,1)
                windows.mouse_button.assert_not_called()

    def test_gdi_capture_cleanup(self):
        for succeeds in (True,False):
            with self.subTest(succeeds=succeeds):
                windows = Windows.__new__(Windows)
                windows.u,windows.g = mock.Mock(),mock.Mock()
                windows.focus = mock.Mock()
                windows.visible_client = mock.Mock(return_value={'hwnd':100,'width':1,'height':1,'screen_left':10,'screen_top':20})
                pixels = ct.create_string_buffer(b'\x01\0\0\0')
                windows.u.GetDC.return_value = 101
                windows.g.CreateCompatibleDC.return_value = 102
                def dib(dc,info,usage,bits,section,offset):
                    header = ct.cast(info,ct.POINTER(BitmapInfo)).contents.header
                    self.assertEqual((header.size,header.width,header.height,header.bits),(40,1,-1,32))
                    ct.cast(bits,ct.POINTER(ct.c_void_p))[0] = ct.addressof(pixels)
                    return 103
                windows.g.CreateDIBSection.side_effect = dib
                windows.g.SelectObject.side_effect = [104,103]
                windows.g.BitBlt.return_value = int(succeeds)
                windows.g.GdiFlush.return_value = 1
                if succeeds:
                    result = windows.capture(42,1280,mock.Mock())
                    self.assertEqual(result['images'][0]['eye'],'window')
                    self.assertTrue(base64.b64decode(result['images'][0]['png_base64']).startswith(b'\x89PNG'))
                else:
                    with self.assertRaisesRegex(ValueError,'copy failed'): windows.capture(42,1280,mock.Mock())
                self.assertEqual(windows.g.method_calls[-3:],
                                 [mock.call.SelectObject(102,104),mock.call.DeleteObject(103),mock.call.DeleteDC(102)])
                windows.u.ReleaseDC.assert_called_once_with(None,101)

    def test_transport_job_results(self):
        host,_,_ = self.fixture()
        transport = mock.Mock()
        transport.complete.return_value = 'delivered'
        transport.read.side_effect = [None,{'id':1,'name':'xr_sim_capture','arguments':{}},None,
                                      {'id':2,'name':'unsafe_exec','arguments':{}},KeyboardInterrupt()]
        events = deque(maxlen=128)
        with self.assertRaises(KeyboardInterrupt): serve(transport,host,None,events)
        self.assertEqual(transport.complete.call_args_list[0].args[1]['data']['images'][0]['png_base64'],'real-png')
        self.assertIn('error',transport.complete.call_args_list[1].args[1])
        self.assertEqual(len(events),2)

    def test_late_completion_preserves_inspection_without_replaying_effects(self):
        host,_,_ = self.fixture()
        host.keyboard.sequence = mock.Mock(return_value={'delivery_queued':True})
        transport = Transport.__new__(Transport)
        transport.dll = mock.Mock()
        transport.dll.modlinker_mcp_complete.side_effect = [-3,0]
        transport.read = mock.Mock(side_effect=[None,{'id':1,'name':'keyboard_input','arguments':{'keys':['W'],'duration_ms':1}},
                                               None,{'id':2,'name':'get_state','arguments':{}},KeyboardInterrupt()])
        events = deque(maxlen=128)
        with self.assertRaises(KeyboardInterrupt): serve(transport,host,None,events)
        host.keyboard.sequence.assert_called_once()
        self.assertEqual(host.completion_unknown_count,1)
        self.assertEqual([event['completion'] for event in events],['unknown','delivered'])
        second = json.loads(transport.dll.modlinker_mcp_complete.call_args_list[1].args[1])
        self.assertTrue(second['data']['alive'])
        self.assertEqual(second['data']['completion_unknown_count'],1)
        for code in (-1,-2):
            transport.dll.modlinker_mcp_complete.side_effect = None
            transport.dll.modlinker_mcp_complete.return_value = code
            with self.assertRaisesRegex(ValueError,'MCP completion failed'):
                transport.complete(3,{})

    def test_dry_run_and_duplicate_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('transport.dll','client.exe','EliteDangerous64.exe'):
                (root/name).write_bytes(b'fixture')
            args = ['--transport-dll',str(root/'transport.dll'),'--client',str(root/'client.exe'),
                    '--game-dir',str(root),'--expect-scene-pid','42','--output',str(root/'new'),'--dry-run']
            before = sorted(p.name for p in root.iterdir())
            with mock.patch.object(Windows,'__init__',side_effect=AssertionError('must not initialize')):
                for value in ('0','1500','2000'):
                    self.assertEqual(main(args+['--input-settle-ms',value]),0)
                for value in ('-1','2001'):
                    self.assertEqual(main(args+['--input-settle-ms',value]),2)
                self.assertEqual(sorted(p.name for p in root.iterdir()),before)
                (root/'new').mkdir()
                self.assertEqual(main(args),2)


if __name__ == '__main__':
    sys.exit(main())
