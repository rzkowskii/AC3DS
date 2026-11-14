# Nintendo 3DS family hardware, for AC3DS

This is a working reference for the hardware AC3DS runs on: the New Nintendo 3DS family, the New 2DS XL in particular. Each section ends with what it means for this port. Facts come from 3dbrew, libctru and Citro3D headers, and the port's own handheld crash dumps. Hardware-only behavior is marked where it differs from Azahar.

## Model family

| Model | Code | Year | CPU | FCRAM | L2 | 3D | C-Stick, ZL/ZR | Screens (top / bottom) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Nintendo 3DS | CTR-001 | 2011 | 2× ARM11, 268 MHz | 128 MB | No | Yes | Circle Pad Pro accessory | 3.53″ / 3.00″ |
| Nintendo 3DS XL | SPR-001 | 2012 | 2× ARM11, 268 MHz | 128 MB | No | Yes | Circle Pad Pro accessory | 4.88″ / 4.18″ |
| Nintendo 2DS | FTR-001 | 2013 | 2× ARM11, 268 MHz | 128 MB | No | No, one panel | No | 3.53″ / 3.00″ |
| New Nintendo 3DS | KTR-001 | 2014 | 4× ARM11, up to 804 MHz | 256 MB | 2 MB | Yes, face-tracked | Built in | 3.88″ / 3.33″ |
| New Nintendo 3DS XL | RED-001 | 2015 | 4× ARM11, up to 804 MHz | 256 MB | 2 MB | Yes, face-tracked | Built in | 4.88″ / 4.18″ |
| New Nintendo 2DS XL | JAN-001 | 2017 | 4× ARM11, up to 804 MHz | 256 MB | 2 MB | No | Built in | 4.88″ / 4.18″ |

Every model has the same GPU (DMP PICA200 at 268 MHz), the same 6 MB of VRAM, and the same resolutions. The top screen is 400×240 per eye, or 800×240 in wide mode. The bottom screen is 320×240 with a resistive touch panel, so only one touch point is reported. The New models add CPU cores, CPU clock, L2 cache and RAM. They don't add GPU power.

**For AC3DS:** The port requires a New model (`ac3ds_system_init` checks `APT_CheckNew3DS`). The New 2DS XL and the New 3DS XL are the same machine apart from the 3D panel and face-tracking camera. A frame bound by GPU work runs no faster on a New model, and only CPU-side work benefits from the boost.

## CPU: ARM11 MPCore (ARMv6K) with VFPv2

- **Cores (New models):** core 0 is the application core. Core 1 is the system core; an app can borrow part of it with `APT_SetAppCpuTimeLimit`. Core 2 is a second application core on New models only. Core 3 belongs to the system.
- **Clock:** 268 MHz by default. On New models, `osSetSpeedupEnable(true)` requests 804 MHz and turns on the 2 MB L2 cache, but only for the running application. `svcGetSystemTick` always counts at 268,111,856 Hz (`SYSCLOCK_ARM11`), whatever the CPU clock.
- **Caches:** each core has a 16 KB instruction cache and a 16 KB data cache. New models add a 2 MB shared L2 (an L2C-310 controller), which is active only while speedup is on.
- **FPU:** VFPv2, single and double precision, with no NEON or other SIMD. Single precision is the fast path.
- **Alignment (hardware-only failure):** with unaligned support enabled, single-register `LDR`/`STR`/`LDRH`/`STRH` tolerate any address. `LDM`/`STM`, `LDRD`/`STRD` and VFP `VLDR`/`VSTR`/`VLDM`/`VSTM` need at least 4-byte alignment and raise an alignment fault otherwise. GCC emits these freely for struct copies and float vectors (for example, `vec` assignment becomes `ldm`/`stm`). Azahar doesn't fault.

**For AC3DS:**
- The handheld crash fixed on 2026-11-14 was exactly this. Luma dump: data abort, DFSR `0x801` (alignment fault on a write), `stm` storing a `vec` into a model-cache entry at `0x08311b8a`.
- Any byte-packed buffer that is cast to a struct is a candidate for the same crash: custom allocators, network packets, file formats read in place. Use `memcpy`, or keep the offsets 4-byte aligned.
- The audio mixer and the crash watchdog run on core 2. Main-thread work that reaches 804 MHz is CPU-bound; GPU waits are not.

## Memory

### Physical map (ARM11 view)

| Physical address | Size | What |
| --- | --- | --- |
| `0x18000000` | 6 MB | VRAM, in two halves (A and B) |
| `0x1F000000` | 4 MB | New-only extra memory. Only the first 2 MB is GPU-visible; QTM and the kernel use part of the rest. |
| `0x1FF00000` | 512 KB | DSP memory |
| `0x1FF80000` | 512 KB | AXI WRAM, used by the kernel |
| `0x20000000` | 128 MB | FCRAM |
| `0x28000000` | 128 MB | New-only FCRAM extension (256 MB total) |

### Application memory modes (APPMEMTYPE)

FCRAM is split into APPLICATION, SYSTEM and BASE regions, and the split is fixed at boot.

| APPMEMTYPE | APPLICATION size | Where |
| --- | --- | --- |
| 0 | 64 MB | Old 3DS default |
| 2 / 3 / 4 / 5 | 96 / 80 / 72 / 32 MB | Old 3DS alternatives |
| 6 (and 8) | 124 MB | New 3DS default |
| 7 | 178 MB | New 3DS extended (Luma calls it a "dev" layout, and Home Menu can crash) |

An application gets the larger New-model regions only when its exheader asks for them. A `.3dsx` inherits whatever the title it took over was given. With Luma's Homebrew Launcher, that is usually Download Play or another takeover title. The port logs what it actually got at startup: `memory: appmemtype …, application … KB`.

### Virtual layout inside the app

| Virtual address | What |
| --- | --- |
| `0x00100000` | `.text`, `.rodata`, `.data`, `.bss` |
| `0x08000000` | Normal heap (`malloc`/`new`) |
| `0x10000000 - stack` | Main-thread stack. AC3DS sets `__stacksize__ = 1 MB`. |
| `0x14000000` or `0x30000000` | LINEAR heap: physically contiguous and GPU-addressable. The New kernel maps up to 256 MB at `0x30000000`. |
| `0x1F000000` | VRAM mapping |

**For AC3DS:**
- `__ctru_linear_heap_size = 48 MB` (`source/3ds/system.cpp`). Everything the GPU reads must live in the linear heap or VRAM: vertex and index buffers, textures, command lists. Normal heap memory isn't physically contiguous and isn't GPU-addressable.
- If the takeover title only grants 64 MB, a 48 MB linear heap leaves about 16 MB for code, heap and stacks, and the game won't fit. Check the startup `memory:` line in each handheld log.
- The GPU addresses memory physically, `>> 3`, in 28 bits. Citro3D's `C3D_BufInfo` base must sit at or below the lowest buffer, and every offset must fit in 28 bits. The port uses `OS_FCRAM_PADDR` as the base.

## GPU: DMP PICA200

### Pipeline

- **Vertex stage:** three programmable vertex processors plus a geometry-shader unit (which can act as a fourth). The shaders use one ISA, assembled by Picasso. Limits: 96 float vec4 uniforms, 4 int and 16 bool uniforms, up to 16 input attributes (12 vertex arrays), and 7 output registers mapped to fixed semantics (position, color, texcoord0–2, texcoord0.w, normquat, view).
- **Fixed-function fragment stage:** 6 texture-combiner (TEV) stages plus a combiner buffer, per-fragment lighting with lookup tables, fog, alpha/stencil/depth tests and blending. There are no fragment shaders.
- **Texture units:** units 0–2 are normal. Unit 0 alone can do cube maps, shadow maps and projective lookups. Unit 3 is procedural only.
- **Textures:** each dimension is a power of two from 8 to 1024. Texels are stored in 8×8 Morton-tiled blocks. Formats: RGBA8, RGB8, RGBA5551, RGB565, RGBA4, IA8, HILO8, I8, A8, IA4, I4, A4, ETC1, and ETC1A4.
- **Render targets:** color in RGBA8, RGB8, RGB565, RGB5A1 or RGBA4. Depth in D16, D24 or D24S8. Framebuffers are stored rotated 90° (240×400 for the top screen) and tiled. The transfer engine detiles them into the LCD framebuffers.

### Command processing

- The GPU runs command lists from physical memory. Address and size are `>> 3`, so lists must be 8-byte aligned and padded. Every list must end with a `GPUREG_FINALIZE` write; without it, the GPU hangs. Citro3D adds this in `C3D_FrameEnd`/`C3D_FrameSplit`.
- Completion raises the P3D interrupt. GSP relays it, and Citro3D's `gxCmdQueue` waits on it. The next `C3D_FrameBegin` blocks until the previous queue drains.
- GPU status register `0x10400034` (read through `GSPGPU_ReadHWRegs(0x400034, …)`): bit 26 is PSC0 (memory fill 0), bit 27 is PSC1, bit 30 is PPF (the transfer engine), and bit 31 is P3D (the 3D command processor).
- **Transfer engine (`GX_DisplayTransfer`, `GX_TextureCopy`):**
  - Tiled-to-linear output must be no larger than the input, at least 64 wide and at least 16 high.
  - Widths must be 8-byte aligned, or 16-byte aligned for RGB8.
  - Some format conversions never raise a completion interrupt, so anything waiting on them hangs. Examples: RGB8→RGBA8, RGB565→RGBA8, RGB565→RGB8, RGB5A1→RGBA8, RGBA4→RGBA8. RGBA8 to any output format is safe.

### CPU and GPU coherency (hardware-only failure)

The ARM11 data cache isn't coherent with the GPU.
- After the CPU writes anything the GPU will read (vertices, indices, texels, command lists), it must call `GSPGPU_FlushDataCache` on that range.
- Before the CPU reads GPU output (`readTarget`, the aim tile), it must call `GSPGPU_InvalidateDataCache`.

Azahar has no cache model, so a missing flush works there and fails on hardware with stale data, garbage or a hang.

**For AC3DS:**
- All rendering goes through Citro3D in `source/3ds/ac3ds_gl.cpp`. Textures are scaled up to the power-of-two, 8-pixel minimum, and render targets live in VRAM. Its existing flush calls match the rule above.
- The PICA200 runs at the same 268 MHz on every model. Fill rate, vertex throughput and transfer bandwidth are fixed budgets, so the CPU boost doesn't help GPU-bound frames.
- The 2026-11-14 handheld stall report read `gpu busy=80000000`. That is bit 31, the P3D command processor, busy during `load map` 20 seconds into the stall. Azahar doesn't report that state. If the next handheld run stalls again in the same place, suspect a GPU command list that never finishes during the load-screen draws rather than slow I/O.
- ETC1 compression (4 bits per pixel for opaque textures) is available and isn't used yet. It would cut linear-heap use and texture bandwidth compared with RGB565.

## VRAM

There are 6 MB, in two 3 MB halves. Only the GPU and the transfer engine can render into it efficiently, and `vramAlloc` hands it out.

**For AC3DS:** the top target (400×240 RGBA8 color plus D24S8 depth, about 750 KB), the water reflection and refraction targets (128×128), shadow targets and the water VRAM snapshot share the 6 MB. Large render-to-texture work belongs here. Static textures stay in the linear heap.

## Audio: CEVA TeakLite DSP

- 134 MHz, 24 voices, natively 32,728 Hz. libctru's `ndsp` drives it.
- It needs the console's DSP firmware dumped to `sdmc:/3ds/dspfirm.cdc` (Luma's DSP1 tool). Without it, `ndspInit` fails and the port continues without sound.

**For AC3DS:** see `docs/audio-3ds.md`. Mixing at the display rate costs CPU on core 2.

## Input and other hardware

- **New models:** Circle Pad, C-Stick and ZL/ZR are read through HID directly. On older models, the C-Stick and ZL/ZR came from the Circle Pad Pro through `ir:rst`. Gyroscope and accelerometer are present. NFC is built into New models.
- **Touch:** a single resistive point, read through HID `hidTouchRead`.
- **Wi-Fi:** 802.11b/g through the `soc:U` service, which needs a page-aligned buffer donated from the app heap.
- **New 2DS XL specifics:** no 3D panel and no face-tracking camera use. The top and bottom screen resolutions are unchanged.

## Storage and I/O

- The SD card is FAT32, accessed through the FS service by IPC. Every call has high fixed latency, so throughput depends on large sequential reads.
- A `.3dsx` with an embedded RomFS is read from the SD card on every RomFS access.
- File I/O in Azahar goes to the host disk and is much faster than on hardware.

**For AC3DS:** in Azahar, the first map load takes 22–26 seconds, of which map models are 13–17 seconds. On hardware, I/O-heavy stages may take longer. Batching asset reads and caching parsed models would help most.

## The homebrew environment

- **Custom firmware:** Luma3DS. Rosalina loads `.3dsx` files through the Homebrew Launcher, using a "takeover" title such as Download Play. The takeover title's memory mode and service access apply to the homebrew.
- **Crash reporting:** Luma writes ARM11 dumps to `/luma/dumps/arm11/crash_dump_*.dmp`. The header holds the version, processor and core (`u16` each) and the exception type, where 3 is a data abort. Then come the registers r0–r12, sp, lr, pc, cpsr, DFSR, IFSR, FAR, FPEXC and the FPINST registers, followed by code and stack dumps. The port also installs its own per-thread handler (`source/3ds/crash_3ds.cpp`), which writes `profile/crash.txt`.
- **Exception status codes:** in DFSR, bits 0–3 are the fault status and bit 11 is write-not-read. Status `0001` is an alignment fault, `0101`/`0111` are translation faults (unmapped), and `1101`/`1111` are permission faults.

## Azahar differences to keep in mind

| Area | Azahar | Hardware |
| --- | --- | --- |
| Unaligned `ldm`/`stm`/`ldrd`/VFP | Works | Alignment fault (crash) |
| Access to unmapped memory | Logged, then continues | Data abort (crash) |
| Missing cache flush or invalidate | Works | Stale or garbage data, or a GPU hang |
| Invalid or incomplete GPU command list | Often renders or skips | P3D hang, so `C3D_FrameBegin` blocks forever |
| GPU cost | Mostly unmodeled | Fixed 268 MHz budget |
| SD and RomFS I/O | Host disk speed | FS IPC latency and SD bandwidth |
| New 3DS 804 MHz boost | Not implemented (logged) | Available |
| Memory mode | As configured | Depends on how the takeover title was launched |

Azahar passing doesn't establish hardware behavior. Every claim in `docs/port-status.md` says which one it is based on.

## Sources

- 3dbrew: [Hardware](https://www.3dbrew.org/wiki/Hardware), [Memory layout](https://www.3dbrew.org/wiki/Memory_layout), [GPU internal registers](https://www.3dbrew.org/wiki/GPU/Internal_Registers), [GPU external registers](https://www.3dbrew.org/wiki/GPU/External_Registers)
- libctru and Citro3D headers under `.deps/devkitpro/libctru/include`: `os.h` (memory types), `errf.h` and `thread.h` (exception handling), `gpu/`
- Luma3DS memory layout options: [3ds.hacks.guide config](https://3ds.hacks.guide/assets/config.ini), [Luma3DS](https://github.com/LumaTeam/Luma3DS)
- ARMv6 alignment model: [ARM1176JZF-S TRM, chapter 4](https://documentation-service.arm.com/static/5e8e294efd977155116a6ca3)
- Port evidence: Luma dumps and logs under `build/logs/hardware-sd-*`
