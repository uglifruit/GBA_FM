# screenshots.py — render the manual's screenshots by running the REAL payload under emulation.
#
# The payload draws in mode 3, so VRAM at 0x06000000 IS the 240x160 RGB555 framebuffer: no PPU
# emulation is needed, only the ARM. unicorn runs build/payload.elf from the multiboot entry; the
# I/O registers are a plain register file except the few the payload waits or paces on, a host
# that has said HELLO is faked by writing the link globals, buttons are pressed between
# ui_frame() calls, and fm_render_block() is called between frames so the meters move.
# Run after ./build.sh, whenever the UI changes:
#
#   pip install unicorn pillow
#   python screenshots.py [payload dir] [out dir]      (defaults: here, ../docs/images/screens)
#
# The DSP/TICK LOAD figures on CAL are set by hand (nothing here models real timing).
import os, shutil, struct, subprocess, sys
from unicorn import *
from unicorn.arm_const import *
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
PAY = sys.argv[1] if len(sys.argv) > 1 else HERE
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', 'docs', 'images', 'screens')
os.makedirs(OUT, exist_ok=True)
NM = shutil.which('arm-none-eabi-nm') or os.path.expanduser('~/.pico-sdk/toolchain/14_2_Rel1/bin/arm-none-eabi-nm.exe')

sym = {}
for ln in subprocess.run([NM, os.path.join(PAY, 'build/payload.elf')], capture_output=True, text=True).stdout.splitlines():
    p = ln.split()
    if len(p) == 3: sym.setdefault(p[2], int(p[0], 16))

uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
for base, size in [(0, 0x4000), (0x02000000, 0x40000), (0x03000000, 0x8000),
                   (0x05000000, 0x1000), (0x06000000, 0x18000), (0x07000000, 0x1000)]:
    uc.mem_map(base, size)
uc.mem_write(0x02000000, open(os.path.join(PAY, 'build/payload.mb'), 'rb').read())

# ---- I/O: plain register file, except the few registers the payload waits or paces on ----
io = bytearray(0x1000)
st = dict(vc=0, tm2=0, tm3=0, keys=0)
def io_read(uc, off, size, ud):
    if off == 0x006:                       # VCOUNT: alternate, so each frame wait passes at once
        st['vc'] ^= 1; return 160 if st['vc'] else 0
    if off == 0x108:                       # Timer 2: one control tick per synth_update call
        st['tm2'] = (st['tm2'] + 16) & 0xFFFF; return st['tm2']
    if off == 0x10C:                       # Timer 3: a tick costs ~40 % of its 1 ms
        st['tm3'] = (st['tm3'] + 6554) & 0xFFFF; return st['tm3']
    if off == 0x130:                       # KEYINPUT, active low
        return (~st['keys']) & 0x3FF
    v = int.from_bytes(io[off:off + size], 'little')
    if off == 0x128: v |= 0x80             # SIO start bit stays set: no word ever completes
    return v
def io_write(uc, off, size, val, ud):
    io[off:off + size] = val.to_bytes(size, 'little')
uc.mmio_map(0x04000000, 0x1000, io_read, None, io_write, None)

def w8(n, v, i=0):  uc.mem_write(sym[n] + i, struct.pack('<B', v))
def w16(n, v, i=0): uc.mem_write(sym[n] + 2 * i, struct.pack('<H', v))
def w32(n, v, i=0): uc.mem_write(sym[n] + 4 * i, struct.pack('<I', v))
def r32(n, i=0):    return struct.unpack('<I', uc.mem_read(sym[n] + 4 * i, 4))[0]

# ---- stop at main (to fake a talking host) and at every ui_frame ----
UI = sym['ui_frame']
flags = dict(skip=False, stop=None)
def on_code(uc, addr, size, ud):
    if flags['skip']:
        flags['skip'] = False; return
    flags['stop'] = addr
    uc.emu_stop()
uc.hook_add(UC_HOOK_CODE, on_code, begin=sym['main'], end=sym['main'])
uc.hook_add(UC_HOOK_CODE, on_code, begin=UI, end=UI)
uc.hook_add(UC_HOOK_INTR, lambda uc, n, ud: None)

SENT, BUF = 0x0203FF00, 0x0203F000
def call(fn, *args):
    ctx = uc.context_save()
    for r, a in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), args): uc.reg_write(r, a)
    uc.reg_write(UC_ARM_REG_LR, SENT)
    uc.reg_write(UC_ARM_REG_SP, uc.reg_read(UC_ARM_REG_SP) - 0x200)
    uc.emu_start(sym[fn], SENT, count=5_000_000)
    uc.context_restore(ctx)

uc.reg_write(UC_ARM_REG_CPSR, 0xDF)       # System mode, IRQ/FIQ masked, as the BIOS hands over
uc.reg_write(UC_ARM_REG_SP, 0x03007F00)
uc.emu_start(0x020000C0, 0, count=50_000_000)
assert flags['stop'] == sym['main'], hex(uc.reg_read(UC_ARM_REG_PC))
w32('g_rx', 1)
flags['skip'] = True
uc.emu_start(sym['main'], 0, count=200_000_000)
assert flags['stop'] == UI

# A host that has said HELLO and is streaming: the factory Workshop with CV out calibration and
# audio-input edges, five saved slots, a note on CV2 and a fifth above it on Audio In 2.
w16('g_hostCaps', 0x3); w8('g_hostSeen', 1)
w16('g_slotMask', 0b0000000100100111)
w16('g_in', 2300, 0); w16('g_in', 2048 + 341 - 20, 1); w16('g_in', 2048, 2); w16('g_in', 2048 + 341 + 199 - 25, 3)
for i in range(3): w16('g_knob', 2048, i)
w8('g_switch', 1)

frame = 0
def step(keys=0, n=1, render=34):
    """Run n frames with `keys` held, feeding the audio engine `render` blocks per frame."""
    global frame
    for _ in range(n):
        st['keys'] = keys
        for _ in range(render): call('fm_render_block', BUF, BUF + 16)
        w32('g_rx', r32('g_rx') + 33); w32('g_streamRx', r32('g_streamRx') + 33)
        w32('g_irqCount', r32('g_irqCount') + 33)
        w16('g_dspAvg', 30 * 16); w16('g_dspPeak', 34 * 16)
        w16('g_tickPeak', 63 * 16)
        flags['skip'] = True
        uc.emu_start(UI, 0, count=400_000_000)
        assert flags['stop'] == UI, hex(uc.reg_read(UC_ARM_REG_PC))
        frame += 1

def gate(on):
    w8('g_gate', 1 if on else 0)
    w8('g_audLevel', 1 if on else 0)
    if on:
        w32('g_gateEdges', r32('g_gateEdges') + 1); w32('g_audEdges', r32('g_audEdges', 0) + 1, 0)

def shot(name):
    px = struct.unpack('<%dH' % (240 * 160), uc.mem_read(0x06000000, 240 * 160 * 2))
    img = Image.new('RGB', (240, 160))
    img.putdata([((c & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31, ((c >> 10) & 31) * 255 // 31) for c in px])
    img.resize((480, 320), Image.NEAREST).save(os.path.join(OUT, f'screen-{name}.png'))
    print('wrote', name, 'frame', frame)

KA, KSEL, KSTART, KR, KL, KU, KD = 1, 4, 8, 16, 32, 64, 128
def press(k, hold=0):
    step(hold | k); step(hold); step(hold)
def next_page():
    step(KSEL); step(KSEL | KR); step(KSEL); step(0, 3)

step(0, 2); gate(True); step(0, 8); shot('play')
press(KSTART); step(0, 2); shot('mem')
for name in ['fm', 'env', 'mix', 'map', 'btn', 'set', 'cal']:
    next_page(); shot(name)

# 4-op: SET is two pages back from CAL; VOICE MODE is its first row.
step(KSEL); step(KSEL | KL); step(KSEL); step(0, 3)
step(KA); step(KA | KU); step(KA); step(0, 3)
gate(False); step(0, 4); press(KSTART); step(0, 2); gate(True); step(0, 8); shot('play-4op')
press(KSTART); step(0, 2)
next_page(); shot('fm-4op')
next_page(); shot('env-4op')
next_page()

# Drum mode, back in two-voice mode: SET is three pages on from MIX.
for _ in range(3): next_page()
step(KA); step(KA | KD); step(KA); step(0, 2)          # VOICE MODE back to 2 VOICES
press(KD); step(KA); step(KA | KU); step(KA); step(0, 3)
gate(False); step(0, 30); press(KSTART); step(0, 2)
gate(True); w32('g_audEdges', r32('g_audEdges', 1) + 1, 1); w8('g_audLevel', 3)
step(0, 3, render=20); shot('play-drums')
