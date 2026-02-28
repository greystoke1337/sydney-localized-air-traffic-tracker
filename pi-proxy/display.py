import os
import sys
import time
import numpy as np
import pygame
import requests

# Offscreen rendering -- we write pixels directly to /dev/fb1
os.environ['SDL_VIDEODRIVER'] = 'offscreen'
os.environ['SDL_NOMOUSE']     = '1'

W, H        = 480, 320
REFRESH     = 10
FB_DEV      = '/dev/fb1'
STATS_URL   = 'http://localhost:3000/stats'
PEAK_URL    = 'http://localhost:3000/peak'

BG     = (15,  15,  25)
ACCENT = (0,  180, 255)
GREEN  = (0,  220, 100)
AMBER  = (255,180,  0)
RED    = (255, 80,  80)
DIM    = (80,  90, 110)
WHITE  = (230,235, 245)

pygame.init()
screen = pygame.display.set_mode((W, H))

font_lg = pygame.font.SysFont("monospace", 26, bold=True)
font_md = pygame.font.SysFont("monospace", 18)
font_sm = pygame.font.SysFont("monospace", 13)

# Open framebuffer once
fb = open(FB_DEV, 'wb')

def flush_to_fb():
    arr = pygame.surfarray.array3d(screen).transpose(1, 0, 2)  # (H, W, 3)
    r = arr[:, :, 0].astype(np.uint16)
    g = arr[:, :, 1].astype(np.uint16)
    b = arr[:, :, 2].astype(np.uint16)
    rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    fb.seek(0)
    fb.write(rgb565.astype('<u2').tobytes())
    fb.flush()

def fetch():
    try:
        s = requests.get(STATS_URL, timeout=3).json()
        p = requests.get(PEAK_URL,  timeout=3).json()
        return s, p
    except Exception:
        return None, None

def draw_bar(x, y, w, h, value, max_val, is_current=False, is_peak=False):
    pygame.draw.rect(screen, (40, 45, 60), (x, y, w, h))
    if not max_val or not value:
        return
    filled = int((value / max_val) * h)
    col = AMBER if is_current else (GREEN if is_peak else ACCENT)
    pygame.draw.rect(screen, col, (x, y + h - filled, w, filled))

def render(stats, peak):
    screen.fill(BG)

    # Title bar
    t = font_lg.render('OVERHEAD TRACKER', True, ACCENT)
    screen.blit(t, (W // 2 - t.get_width() // 2, 8))
    pygame.draw.circle(screen, GREEN if stats else RED, (W - 14, 14), 6)
    pygame.draw.line(screen, DIM, (10, 36), (W - 10, 36), 1)

    if stats is None:
        msg = font_md.render('-- proxy unreachable --', True, RED)
        screen.blit(msg, (W // 2 - msg.get_width() // 2, H // 2))
        flush_to_fb()
        return

    # Stats grid (2 rows x 3 cols)
    items = [
        ('UPTIME',    stats.get('uptime', '-')),
        ('REQUESTS',  str(stats.get('totalRequests', 0))),
        ('CACHE HIT', stats.get('cacheHitRate', '-')),
        ('ERRORS',    str(stats.get('errors', 0))),
        ('CLIENTS',   str(stats.get('uniqueClients', 0))),
        ('CACHED',    str(stats.get('cacheEntries', 0)) + ' entries'),
    ]
    col_w = W // 3
    for i, (label, value) in enumerate(items):
        cx = (i % 3) * col_w + col_w // 2
        cy = 50 + (i // 3) * 52
        lbl = font_sm.render(label, True, DIM)
        err_row = label == 'ERRORS' and int(stats.get('errors', 0)) > 0
        val = font_md.render(value, True, RED if err_row else WHITE)
        screen.blit(lbl, (cx - lbl.get_width() // 2, cy))
        screen.blit(val, (cx - val.get_width() // 2, cy + 16))

    pygame.draw.line(screen, DIM, (10, 162), (W - 10, 162), 1)

    # Peak hour label
    hdr = font_sm.render('TRAFFIC BY HOUR', True, DIM)
    screen.blit(hdr, (10, 168))

    if peak:
        hours   = peak['hours']
        max_cnt = max(h['count'] for h in hours) or 1
        bar_w   = (W - 20) // 24
        chart_h = H - 188 - 20
        chart_y = 185

        for h in hours:
            x          = 10 + h['hour'] * bar_w
            is_current = h['current']
            is_peak    = h['count'] == max_cnt and max_cnt > 0
            draw_bar(x, chart_y, bar_w - 1, chart_h,
                     h['count'], max_cnt, is_current, is_peak)

        for hr in [0, 6, 12, 18]:
            x   = 10 + hr * bar_w
            lbl = font_sm.render(str(hr), True, DIM)
            screen.blit(lbl, (x, H - 18))

    flush_to_fb()

last_fetch = 0
stats_data, peak_data = None, None

while True:
    now = time.time()
    if now - last_fetch >= REFRESH:
        stats_data, peak_data = fetch()
        last_fetch = now
    render(stats_data, peak_data)
    time.sleep(1)