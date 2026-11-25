"""RAM-only iterations from the verified fight checkpoint or a cold game reset."""
import argparse,hashlib,json,pathlib,shlex,time,uuid
from usb_client import Client
BASE=pathlib.Path(__file__).resolve().parents[2]
OUT=BASE/'build/acm/hardware';OUT.mkdir(parents=True,exist_ok=True)
p=argparse.ArgumentParser();p.add_argument('--frames',type=int,default=30);p.add_argument('--interactive',action='store_true');p.add_argument('--profile',action='store_true');p.add_argument('--batched',action='store_true');p.add_argument('--mode',choices=['full','simulation','graphics'],default='full');p.add_argument('--sync-gpu',action='store_true',default=True);p.add_argument('--async-gpu',action='store_false',dest='sync_gpu');p.add_argument('--state',type=pathlib.Path,default=BASE/'build/acm/fight.checkpoint')
p.add_argument('--binary',type=pathlib.Path,default=BASE/'build/acm/bin/vf2-acm')
p.add_argument('--gpu2d',action='store_true')
p.add_argument('--fixed-atlas-luma',action=argparse.BooleanOptionalAction,default=None,help='Use one-byte opaque atlas backing images at the original coordinates (default with --fast)')
p.add_argument('--packed-palette-index',action=argparse.BooleanOptionalAction,default=None,help='Use a compact array to look up allocated 3D palette rows (default with --fast)')
p.add_argument('--packed-tile-map',action=argparse.BooleanOptionalAction,default=None,help='Upload decoded tile maps as packed RGBA4444 (default with --fast)')
p.add_argument('--profile-geometry',action='store_true',help='Sample only the owned geometry worker CPU clock')
p.add_argument('--profile-simulation',action='store_true',help='Sample only the simulation thread CPU clock')
p.add_argument('--frame-trace',action='store_true',help='Archive simulation, EGL submission and pacing times for each tick')
p.add_argument('--rotate-tile-maps',action=argparse.BooleanOptionalAction,default=None,help='Alternate packed quadrant tile textures')
p.add_argument('--rotate-3d-palette',action=argparse.BooleanOptionalAction,default=None,help='Alternate wide 3D palette textures')
p.add_argument('--trace-upload-stalls',action='store_true',help='Record uploads and slow renderer submission phases')
p.add_argument('--quiet-monitor',action='store_true',help='Wait out the run before issuing USB status requests')
p.add_argument('--profile-simulation-windows',action='store_true',help='Archive simulation CPU samples by report window')
p.add_argument('--profile-tgp-commands',action='store_true',help='Sample DSP commands while retaining native and resumable kernels')
p.add_argument('--profile-tgp-pcs',action='store_true',help='Diagnostic DSP PC/command CPU-time samples on the simulation thread')
p.add_argument('--extended-matrix',action=argparse.BooleanOptionalAction,default=None,help='Native reverse-matrix entry with a variable left operand')
p.add_argument('--resumable-matrix',action=argparse.BooleanOptionalAction,default=None,help='Native private matrix arithmetic spanning exact scheduler slices')
p.add_argument('--native-matrix',action=argparse.BooleanOptionalAction,default=None,help='Native 3x4 DSP matrix composition (default with --fast)')
p.add_argument('--cooperative-fifo',action='store_true',help='Experimental direct logical i960/DSP FIFO service')
p.add_argument('--timed-cooperative-fifo',action='store_true',help='Experimental direct FIFO service with virtual output readiness')
p.add_argument('--fifo-batch',type=int,choices=(1,2,4,8),default=1,help='Experimental cooperative DSP input-service threshold; shared RAM and slice boundaries flush partial batches')
p.add_argument('--draw-cadence',choices=tuple(f'{draw}/{period}' for period in range(2,9) for draw in range(1,period)),help='Advance gameplay every tick and draw D of every N ticks')
p.add_argument('--frame-budget-division',action=argparse.BooleanOptionalAction,default=None,help='Use the bounded exact quotient in the modular board scheduler')
p.add_argument('--skip-inert-handoffs',action=argparse.BooleanOptionalAction,default=None,help='Skip proven clock-inert passes after a modular timer-head change')
p.add_argument('--trained-dsp-bundle',action=argparse.BooleanOptionalAction,default=None,help='Use the trained geometry DSP fast paths (default in compatible --fast gameplay)')
p.add_argument("--cached-dsp-entry",action="store_true",help="Reuse verified state at a mapped DSP slice boundary")
p.add_argument("--packed-dsp-handlers",action="store_true",help="Use the compact handler-pointer table for validated DSP code")
p.add_argument('--paced',action='store_true',help='Pace a finite run at the arcade rate')
p.add_argument('--boot',action='store_true',help='Start from a cold game reset instead of the fight checkpoint')
p.add_argument('--atlas-state-plan',type=pathlib.Path,help='Use a custom signed preload plan for a cold game reset')
p.add_argument('--no-atlas-preload',action='store_true',help='Disable the bundled texture preload for fast cold starts')
p.add_argument('--input-script',type=pathlib.Path,help='Apply a bounded controller script during the run')
p.add_argument('--compact-packets',action=argparse.BooleanOptionalAction,default=None,help='Cache packet occupancy and spare storage on the preparing worker (default with --fast)')
p.add_argument('--stage-materials',action=argparse.BooleanOptionalAction,default=None,help='Plan palettes, atlas updates and final batches before the GPU fence (default with --fast)')
p.add_argument('--register-cycles',action=argparse.BooleanOptionalAction,default=None,help='Keep the i960 cycle budget in a callee-saved ARM register (default with --fast)')
p.add_argument('--lazy-pc',action=argparse.BooleanOptionalAction,default=None,help='Defer integer-instruction PC bookkeeping until observation boundaries (default with --fast)')
p.add_argument('--overlap-palettes',action=argparse.BooleanOptionalAction,default=None,help='Prepare palette rows alongside 3D packets (default with --fast full rendering)')
p.add_argument('--early-material',action=argparse.BooleanOptionalAction,default=None,help='Omit unused material fetches for rejected native polygons (default with --fast)')
p.add_argument('--direct-geometry',action=argparse.BooleanOptionalAction,default=None,help='Submit complete native geometry polygons directly (default with --fast)')
p.add_argument('--reference-camera-sort',action='store_true',help='Sort camera indices using the original per-comparison polygon reads')
p.add_argument('--direct-projection',action=argparse.BooleanOptionalAction,default=None,help='Project camera polygons directly into their final sorted output (default with --fast camera packets)')
p.add_argument('--shared-camera-colors',action=argparse.BooleanOptionalAction,default=None,help='Reuse renderer snapshot color translation and gamma for camera projection')
p.add_argument('--shared-camera',action=argparse.BooleanOptionalAction,default=None,help='Share immutable camera records (default with --fast compact camera without mesh diagnostics)')
p.add_argument('--compact-camera',action=argparse.BooleanOptionalAction,default=None,help='Keep geometry records compact through the render handoff (default with --fast)')
p.add_argument('--cache-2d-pens',action=argparse.BooleanOptionalAction,default=None,help='Keep decoded System 24 pens until the palette or translation table changes')
p.add_argument('--hash-2d-pens',action='store_true',help='Report the decoded System 24 palette hash each time it is drawn')
p.add_argument('--coalesce-line-scroll',action=argparse.BooleanOptionalAction,default=None,help='Group adjacent tile rows with identical horizontal scroll')
p.add_argument('--gpu-line-scroll-opaque-only',action=argparse.BooleanOptionalAction,default=None,help='Limit GPU line scroll to opaque tile layers')
p.add_argument('--gpu-line-scroll',action=argparse.BooleanOptionalAction,default=None,help='Sample paired System 24 scroll offsets in the tile fragment shader')
p.add_argument('--clip-empty-tiles',action=argparse.BooleanOptionalAction,default=None,help='Skip tile bands with no visible pixels at the requested priority (default with --fast full rendering)')
p.add_argument('--native-repeat-material',action=argparse.BooleanOptionalAction,default=None,
               help='Use native GPU repeat for the profiled VF2 material where polygons cross a seam')
p.add_argument('--atlas-page-cache',action=argparse.BooleanOptionalAction,default=None,help='Validate cached atlas tiles using source-page revisions before scanning words (default with --fast)')
p.add_argument('--homogeneous-atlas-shaders',action=argparse.BooleanOptionalAction,default=None,help='Use simpler shaders for atlas batches made entirely of opaque textured polygons')
p.add_argument('--textured-transparent-shader',action=argparse.BooleanOptionalAction,default=None,help='Use the branch-free textured shader for textured atlas batches that may be transparent')
p.add_argument('--present-at-tick',action=argparse.BooleanOptionalAction,default=None,help='Sleep to the pacing deadline before the swap so presents land on the tick grid')
p.add_argument('--pace-spin-us',type=int,default=None,help='Sleep this many microseconds short of the deadline and spin the rest, to remove wake-up jitter')
p.add_argument('--sound-board',action='store_true',help='Run with the sound board enabled instead of --no-sound-board, to measure its frame cost')
p.add_argument('--audio-experiment',action='store_true',help='Lift the sound-board guards on the pipeline and the checkpoint loader (timing work only)')
p.add_argument('--audio-mute',action='store_true',help='Run the sound board but skip the ALSA write, to separate emulation cost from output cost')
p.add_argument('--scsp-silence',action='store_true',help='Diagnostic: run the sound board but make the SCSP mixer write silence, to separate mixer/DSP cost')
p.add_argument('--halt-68k',action='store_true',help='Diagnostic: run the sound board, timers and scheduler but hold the 68000')
p.add_argument('--force-lazy-aux',action='store_true',help='Diagnostic: take the lazy-aux scheduler path even with the sound board on')
p.add_argument('--no-modular',action='store_true',help='Diagnostic: disable the modular-clock frame fast path')
p.add_argument('--modular-sound',action='store_true',help='Let the modular-clock frame path run with the sound board live (68000 runs in the same pass)')
p.add_argument('--sound-profile',action='store_true',help='Probe: time the 68000 and SCSP+convert parts of the sound board per frame')
p.add_argument('--flat-fragment-probe',action='store_true',help='Diagnostic: remove 3D texture/palette/discard work; intentionally incorrect colours and transparency')
p.add_argument('--atlas-tile-edges',action=argparse.BooleanOptionalAction,default=None,help='Copy atlas borders by row and fill transparent texels in place')
p.add_argument('--projected-stream',action=argparse.BooleanOptionalAction,default=None,help='Feed staged GLES2 from packed projected points and separate polygon materials')
p.add_argument('--compact-cell-vertices',action=argparse.BooleanOptionalAction,default=None,help='Store compact clipped vertices when using the fast indexed renderer')
p.add_argument('--cell-vertex-stats',action='store_true',help='Count full and compact vertices consumed during packet packing')
p.add_argument('--profile-copy-callers',action='store_true',help='Sample preserved ARM memcpy return addresses on the rendering thread')
p.add_argument('--mesh-render',action='store_true',help='Experimental GPU projection for intact ROM polygon fans')
p.add_argument('--mesh-handoff',action='store_true',help='Verify live native mesh identities and transforms')
p.add_argument('--atlas-layout',action='store_true',help='Archive final atlas texture keys and assigned pages for draw analysis')
p.add_argument('--prepared-draws',action=argparse.BooleanOptionalAction,default=None,help='Prepare ordered indexed draw lists on packet workers')
p.add_argument('--early-tile-staging',action=argparse.BooleanOptionalAction,default=None,help='Prepare queued 2D inputs at VBLANK (default in compatible --fast gameplay)')
p.add_argument('--profile-packet-phases',action='store_true',help='Trace packet clipping/palette, planning and packing times')
p.add_argument('--hash-draw-stream',action='store_true',help='Hash the final GLES vertex, index and draw arrays')
p.add_argument('--hash-texture-packets',action='store_true',help='Hash every expanded indexed texture packet for equivalence checks')
p.add_argument('--hash-palette-colours',action='store_true',help='Hash palette bytes in the draw stream, independent of atlas slot')
p.add_argument('--wide-palette-atlas',action=argparse.BooleanOptionalAction,default=None,help='Use two palette banks in one GLES texture')
p.add_argument('--s24-neon-decode',action=argparse.BooleanOptionalAction,default=None,help='Expand System 24 characters with ARM NEON (default with --fast)')
p.add_argument('--palette-shade-cache',action=argparse.BooleanOptionalAction,default=None,help='Reuse RGB shade ramps across palette rows (default with --fast)')
p.add_argument('--profile-palette-churn',action='store_true',help='Archive palette builds and clears by frame')
p.add_argument('--profile-render-stages',action='store_true',help='Account rendering wall/thread CPU stages and prior fence status')
p.add_argument('--cell-index-transforms',action=argparse.BooleanOptionalAction,default=None,help='Reuse integer texture-cell coordinates for ordinary atlas transforms')
p.add_argument('--retained-fan-cell-bounds',action=argparse.BooleanOptionalAction,default=None,help='Use exact cell-edge products to classify retained fans')
p.add_argument('--retained-fans',action=argparse.BooleanOptionalAction,default=None,help='Pack unclipped textured fans directly from their current projected vertices')
p.add_argument('--origin-packet-fan',action=argparse.BooleanOptionalAction,default=None,help='Skip clipping for indexed polygons entirely inside the viewport, mip band and origin texture cell')
p.add_argument('--limit-mip-scan',action=argparse.BooleanOptionalAction,default=None,help='Use integer texture mip limits and stop past the polygon depth range (default with --fast)')
p.add_argument('--keep-inactive-packet-slots',action='store_true',help='Leave inactive packet vectors untouched until reused or trimmed')
p.add_argument('--fused-uv-bounds',action='store_true',help='Scan both projected texture axes together before retaining a fan')
p.add_argument('--fast-viewport-bounds',action=argparse.BooleanOptionalAction,default=None,
               help='Skip redundant clipping for contained polygons with shared projected packets')
p.add_argument('--packet-chunk-capacity',action='store_true',help='Accumulate spare vertex and index capacity on packet workers')
p.add_argument('--shared-packet-storage',action=argparse.BooleanOptionalAction,default=None,help='Store projected draw geometry in shared worker buffers')
p.add_argument('--packet-trim-test',action='store_true',help='Use a low trim limit and report the trim count and capacity checksum')
p.add_argument('--serialize-gpu-passes',action='store_true',help='Diagnostic glFinish after 3D and composition; changes overlap')
p.add_argument('--profile-render-windows',action='store_true',help='Archive render-thread CPU samples by report window')
p.add_argument('--profile-render',action='store_true',help='Sample only the main rendering thread CPU clock')
p.add_argument('--client-arrays',action=argparse.BooleanOptionalAction,default=None,help='Submit CPU vertex/index arrays directly (default with --fast full rendering)')
p.add_argument('--dual-pack',action=argparse.BooleanOptionalAction,default=None,
               help='Use two threads for the final indexed vertex and index packing pass')
p.add_argument('--direct-pack',action=argparse.BooleanOptionalAction,default=None,help='Write packed vertices directly into the final buffers in parallel (default with --fast)')
p.add_argument('--check-direct-pack',action='store_true',help='Compare direct packed vertices, indices and draws with the original path')
p.add_argument('--scalar32',action=argparse.BooleanOptionalAction,default=None,help='Use single precision for exact binary32 add/subtract/multiply forms')
p.add_argument('--i960-direct-branches', action=argparse.BooleanOptionalAction, default=None, help='Emit native i960 branches without hot-path PC stores (default with --fast)')
p.add_argument('--i960-bus-address',action=argparse.BooleanOptionalAction,default=None,help='Reuse computed i960 effective addresses on scalar bus calls (default with --fast)')
p.add_argument('--native-arithmetic',action=argparse.BooleanOptionalAction,default=None,help='Emit i960 scalar floating arithmetic and integer multiply directly as native code')
p.add_argument('--guest-counts',action='store_true',help='Count native guest PCs and TGP instruction attempts (diagnostic timing)')
p.add_argument('--indexed',action=argparse.BooleanOptionalAction,default=None,help='Share triangle fan vertices (default with --fast)')
p.add_argument('--stack-packets',action=argparse.BooleanOptionalAction,default=None,help='Use inline clipping buffers (default with --fast)')
p.add_argument('--pipeline',action='store_true')
p.add_argument('--pipeline-render',action='store_true')
p.add_argument('--geometry-neon',action='store_true')
p.add_argument('--canvas',action='store_true')
p.add_argument('--track-geometry-pages',action=argparse.BooleanOptionalAction,default=None,help='Track display RAM writes and copy only touched pages')
p.add_argument('--geometry-worker',action=argparse.BooleanOptionalAction,default=None,help='Run owned geometry display lists on a worker during vblank (default with --fast camera packets)')
p.add_argument('--camera-packets',action=argparse.BooleanOptionalAction,default=None,help='Prepare clipping/projection from owned camera-space packets (default with --fast full rendering)')
p.add_argument('--modular-clock',action=argparse.BooleanOptionalAction,default=None,help='Experimental bounded low-limb scheduler comparisons with full-width stored clocks')
p.add_argument('--fast-sync-timers',action=argparse.BooleanOptionalAction,default=None,help='Accelerate board FIFO and input-line synchronization timers')
p.add_argument('--scheduler-gaps',action=argparse.BooleanOptionalAction,default=None,help='Use exact narrow scheduler clock differences')
p.add_argument('--i960-timer-reciprocal',action=argparse.BooleanOptionalAction,default=None,help='Use the exact cached divisor for i960 timer reads (default with --fast)')
p.add_argument('--reference-division',action='store_true')
p.add_argument('--reference-duration',action='store_true',help='Use full-width scheduler cycle multiplications')
p.add_argument('--reference-timers',action='store_true')
p.add_argument('--lazy-aux-clocks',action=argparse.BooleanOptionalAction,default=None,help='Frame-boundary advancement for the halted sound CPU (default with --fast)')
p.add_argument('--linear-upscale',action='store_true',help='Smooth the final composed canvas with bilinear magnification')
p.add_argument('--swap-interval',type=int,choices=(0,1),help='Request EGL presentation interval explicitly')
p.add_argument('--reference-packet-math',action='store_true',help='Use libm for repeated quantized texture packet math')
p.add_argument('--reference-geometry-push',action='store_true',help='Dispatch geometry raster commands one word at a time')
p.add_argument('--single-tgp',action='store_true')
p.add_argument('--reference-tgp-counter',action='store_true',help='Update the 64-bit DSP diagnostic counter every instruction')
p.add_argument('--tgp-native-command37',action='store_true',help='Group the private matrix setup and return sequence for DSP command 37')
p.add_argument('--tgp-4a-resume',action=argparse.BooleanOptionalAction,default=None,help='Resume the private DSP span in command 4a after its first two instructions')
p.add_argument('--tgp-native-io',action='store_true',help='Generate DSP I/O transfers for buses with the narrow callback capability (requires --tgp-native-jit)')
p.add_argument('--tgp-vfp-cache',action='store_true',help='Keep translated DSP A/B/D/P in saved VFP registers (requires --tgp-native-jit)')
p.add_argument('--tgp-native-jit',action='store_true',help='Experimental ARM TGP translator with resident host registers for private operations')
p.add_argument('--i960-direct-register-frame',action=argparse.BooleanOptionalAction,default=None,help='Copy aligned i960 register frames directly in work RAM (default with --fast)')
p.add_argument('--i960-port-stores',action=argparse.BooleanOptionalAction,default=None,help='Use the native bus call for repeated VF2 port stores')
p.add_argument("--i960-predecoded-bursts",action=argparse.BooleanOptionalAction,default=None,help="Pass decoded addresses and register groups to the shared multiword transfer helpers")
p.add_argument('--i960-fixed-fifo-load',action=argparse.BooleanOptionalAction,default=None,help='Reuse fixed FIFO address flags across multiword i960 reads')
p.add_argument('--i960-fixed-fifo-store',action=argparse.BooleanOptionalAction,default=None,help='Reuse fixed FIFO address flags across multiword i960 stores')
p.add_argument('--fuse-fifo-halt',action=argparse.BooleanOptionalAction,default=None,help='Deliver a FIFO halt directly when it is the next due timer')
p.add_argument('--fast-halt-delivery',action=argparse.BooleanOptionalAction,default=None,help='Deliver single HALT events directly in the board scheduler (default with fast sync in --fast)')
p.add_argument('--tgp-read-window',action=argparse.BooleanOptionalAction,default=None,help='Read TGP RAM through a contiguous window (default with --fast)')
p.add_argument('--tgp-stable-program',action=argparse.BooleanOptionalAction,default=None,help='Pass the fixed program mapping directly to the DSP (default with --fast)')
p.add_argument('--tgp-resume-dispatch',action=argparse.BooleanOptionalAction,default=None,help='Fuse the TGP dispatch suffix when resuming a slice (default with native dispatch in --fast)')
p.add_argument('--tgp-fused-command-intake',action='store_true',help='Continue from a successful DSP command FIFO read into the native dispatcher')
p.add_argument('--tgp-command32-regions',action='store_true',help='Group private DSP arithmetic inside command 0x32')
p.add_argument('--tgp-native-dispatch',action=argparse.BooleanOptionalAction,default=None,help='Fuse the register-only DSP command dispatch (default with --fast)')
p.add_argument('--tgp-private-regions', action=argparse.BooleanOptionalAction, default=True, help='Combine validated private TGP instruction regions')
p.add_argument('--tgp-blocks',action='store_true',help='Experimental multi-instruction TGP blocks')
p.add_argument('--guarded-tgp',action='store_true',help='Reference per-instruction microcode checks')
p.add_argument('--gpu-queue',action=argparse.BooleanOptionalAction,default=None,help='Bounded GPU overlap (default with --fast full rendering)')
p.add_argument('--atomic-row-claims',action=argparse.BooleanOptionalAction,default=None,help='Use atomic claims for render worker jobs')
p.add_argument('--direct-task-ranges',action=argparse.BooleanOptionalAction,default=None,help='Pass unit task ranges without dividing the row count')
p.add_argument('--render-threads',type=int,choices=range(1,5),help='Total CPU threads preparing render packets')
p.add_argument('--sim-cpu',type=int,choices=range(4),help='Pin simulation and exclude its core from rendering')
p.add_argument('--full-texture-snapshot',action='store_true',help='Reference whole-bank texture snapshot copies')
p.add_argument('--slow-tgp-ram',action='store_true')
p.add_argument('--pgo-core',action=argparse.BooleanOptionalAction,default=None,help='Prefer the source-verified PGO core with --fast; --binary overrides this selection')
p.add_argument('--fast',action='store_true',help='Use the tested compiled-TGP, canvas and render-pipeline configuration')
p.add_argument('--tgp-interpreter',action='store_true')
p.add_argument('--reference-camera-clip',action='store_true',help='Reference camera-plane clipping with vertex copies')
p.add_argument('--native-tgp-rotation',action=argparse.BooleanOptionalAction,default=None,help='Use native transforms for the three known DSP rotation helpers')
p.add_argument('--native-tgp-translation',action=argparse.BooleanOptionalAction,default=None,help='Use native affine translation for the two known DSP helpers')
p.add_argument('--resumable-tgp-rotation',action='store_true',help='Allow private rotation math to span short DSP slices')
a=p.parse_args()
if a.profile_tgp_pcs or a.profile_tgp_commands or a.profile_simulation_windows:a.profile_simulation=True
assert not(a.profile_tgp_pcs and a.profile_tgp_commands)
assert not(a.profile_tgp_commands and (a.tgp_blocks or a.tgp_interpreter)), "Command-only sampling requires the compiled individual-handler path"
if (a.profile_copy_callers and not a.profile_simulation) or a.profile_render_windows:a.profile_render=True
if sum((a.profile_simulation,a.profile_render,a.profile_geometry))>1:p.error('Choose one CPU thread to profile')
if a.profile_simulation or a.profile_render or a.profile_geometry:a.profile=True
if a.quiet_monitor and (a.interactive or not a.paced or a.profile or a.mode!='full'):
    p.error('--quiet-monitor requires a finite paced full run without USB GPU polling')
def verified_pgo():
    directory=BASE/'build/acm/pgo/core';binary=directory/'bin/vf2-acm-tgp'
    try:
        expected=(directory/'binary.sha256').read_text().split()[0]
        if hashlib.sha256(binary.read_bytes()).hexdigest()!=expected:return None
        lines=(directory/'sources.sha256').read_text().splitlines()
        if not lines:return None
        for line in lines:
            expected,name=line.split(maxsplit=1);source=(BASE/name).resolve()
            if not source.is_relative_to(BASE):return None
            if hashlib.sha256(source.read_bytes()).hexdigest()!=expected:return None
        return binary
    except (OSError,ValueError,IndexError):return None
if a.pgo_core is not None and not a.fast:p.error('--pgo-core requires --fast')
if a.fast:
    if a.limit_mip_scan is None:a.limit_mip_scan=a.mode!='simulation'
    if a.tgp_read_window is None:a.tgp_read_window=a.mode!='graphics'
    if a.tgp_stable_program is None:a.tgp_stable_program=a.mode!='graphics'
    if a.packed_palette_index is None:a.packed_palette_index=a.mode!='simulation'
    if a.tgp_native_dispatch is None:a.tgp_native_dispatch=a.mode!='graphics'
    if a.tgp_4a_resume is None:a.tgp_4a_resume=a.tgp_native_dispatch
    if a.tgp_resume_dispatch is None:a.tgp_resume_dispatch=a.tgp_native_dispatch
    if a.i960_direct_register_frame is None:a.i960_direct_register_frame=a.mode!='graphics'
    if a.fixed_atlas_luma is None:a.fixed_atlas_luma=a.mode!='simulation'
    if a.atlas_tile_edges is None:a.atlas_tile_edges=a.mode!='simulation'
    if a.binary==BASE/'build/acm/bin/vf2-acm':
        preferred=verified_pgo() if a.pgo_core is not False else None
        if a.pgo_core and preferred is None:p.error('PGO core is missing or stale; rebuild with tools/acm/build-pgo-arm.sh core')
        a.binary=preferred or BASE/'build/acm/bin/vf2-acm-tgp'
        print('Fast preset binary: '+str(a.binary),flush=True)
    a.batched=True;a.canvas=True;a.pipeline_render=True
    if a.indexed is None:a.indexed=True
    if a.stack_packets is None:a.stack_packets=True
    if a.cell_index_transforms is None:a.cell_index_transforms=a.stack_packets and a.mode!='simulation' and not a.reference_packet_math
    if a.origin_packet_fan is None:a.origin_packet_fan=a.stack_packets and a.mode!='simulation'
    if a.gpu_queue is None:a.gpu_queue=a.mode=='full'
    if a.overlap_palettes is None:a.overlap_palettes=a.mode=='full'
    if a.packed_tile_map is None:a.packed_tile_map=True
    if a.clip_empty_tiles is None:a.clip_empty_tiles=a.mode=='full'
    if a.client_arrays is None:a.client_arrays=a.mode=='full'
    if a.camera_packets is None:a.camera_packets=a.mode=='full'
    if a.geometry_worker is None:a.geometry_worker=bool(a.camera_packets)
    if a.direct_projection is None:a.direct_projection=bool(a.camera_packets)
    if a.lazy_aux_clocks is None:a.lazy_aux_clocks=True
    if a.atomic_row_claims is None:a.atomic_row_claims=a.mode=='full'
    if a.direct_task_ranges is None:a.direct_task_ranges=a.mode=='full'
    if a.direct_pack is None:a.direct_pack=True
    if a.homogeneous_atlas_shaders is None:a.homogeneous_atlas_shaders=a.direct_pack and a.mode=='full'
    if a.present_at_tick is None:a.present_at_tick=a.paced and a.mode=='full'
    if a.pace_spin_us is None:a.pace_spin_us=1000 if (a.paced and a.mode=='full') else None
    if a.dual_pack is None:a.dual_pack=a.mode=='full'
    if a.prepared_draws is None:a.prepared_draws=a.mode=='full'
    if a.native_matrix is None:a.native_matrix=True
    if a.native_tgp_rotation is None:a.native_tgp_rotation=a.native_matrix
    if a.native_tgp_translation is None:a.native_tgp_translation=a.native_matrix
    if a.extended_matrix is None:a.extended_matrix=True
    if a.resumable_matrix is None:a.resumable_matrix=True
    if a.compact_packets is None:a.compact_packets=True
    if a.native_repeat_material is None:a.native_repeat_material=a.mode=='full'
    if a.atlas_page_cache is None:a.atlas_page_cache=True
    if a.compact_camera is None:a.compact_camera=True
    if a.shared_camera is None:a.shared_camera=bool(a.camera_packets and a.compact_camera and not a.mesh_render and not a.mesh_handoff and a.mode=='full')
    if a.shared_camera_colors is None:a.shared_camera_colors=a.shared_camera
    if a.direct_geometry is None:a.direct_geometry=True
    if a.early_material is None:a.early_material=True
    if a.lazy_pc is None:a.lazy_pc=True
    if a.register_cycles is None:a.register_cycles=True
    if a.native_arithmetic is None:a.native_arithmetic=True
    if a.i960_direct_branches is None:a.i960_direct_branches=True
    if a.i960_bus_address is None:a.i960_bus_address=True
    if a.i960_predecoded_bursts is None:a.i960_predecoded_bursts=a.i960_bus_address and a.mode!='graphics'
    if a.scalar32 is None:a.scalar32=True
    if a.stage_materials is None:a.stage_materials=True
    if a.retained_fans is None:
        a.retained_fans=(a.mode=='full' and a.stage_materials and a.direct_pack and a.prepared_draws
                         and a.indexed and a.compact_packets and not a.mesh_render and not a.reference_packet_math)
    if a.compact_cell_vertices is None:a.compact_cell_vertices=bool(a.retained_fans)
    if a.retained_fan_cell_bounds is None:a.retained_fan_cell_bounds=a.retained_fans
    if a.projected_stream is None:
        a.projected_stream=bool(a.mode=='full' and a.retained_fans and a.shared_camera and a.direct_projection
                                and a.gpu_queue and not a.check_direct_pack and not a.reference_camera_sort
                                and not a.mesh_handoff)
    if a.shared_packet_storage is None:a.shared_packet_storage=bool(a.projected_stream)
    if a.fast_viewport_bounds is None:
        a.fast_viewport_bounds=bool(a.projected_stream and a.shared_packet_storage)
    if a.fast_sync_timers is None:a.fast_sync_timers=True
    if a.i960_port_stores is None:a.i960_port_stores=a.mode!='graphics'
    if a.i960_fixed_fifo_load is None:a.i960_fixed_fifo_load=a.mode!='graphics'
    if a.i960_fixed_fifo_store is None:a.i960_fixed_fifo_store=a.mode!='graphics'
    if a.fast_halt_delivery is None:a.fast_halt_delivery=a.fast_sync_timers and a.mode!='graphics'
    if a.fuse_fifo_halt is None:a.fuse_fifo_halt=bool(a.fast_halt_delivery and a.fast_sync_timers)
    if a.scheduler_gaps is None:a.scheduler_gaps=True
    if a.modular_clock is None:a.modular_clock=True
    if a.frame_budget_division is None:a.frame_budget_division=a.modular_clock and not a.reference_division
    if a.skip_inert_handoffs is None:a.skip_inert_handoffs=a.modular_clock
    if a.i960_timer_reciprocal is None:a.i960_timer_reciprocal=not a.reference_division
if a.early_tile_staging is None:
    a.early_tile_staging=bool(a.fast and a.mode=='full' and a.camera_packets and
                              a.geometry_worker and a.gpu_queue and a.modular_clock)
if a.trained_dsp_bundle is None:
    a.trained_dsp_bundle=bool(a.fast and a.mode=='full' and a.tgp_stable_program and
                              a.native_matrix and a.resumable_matrix and a.tgp_native_dispatch and
                              a.tgp_private_regions and not (a.tgp_blocks or a.tgp_interpreter or
                              a.guarded_tgp or a.tgp_native_jit or a.profile_tgp_pcs or a.profile_tgp_commands))
if a.trained_dsp_bundle:
    a.cached_dsp_entry=True
    a.packed_dsp_handlers=True
    a.tgp_fused_command_intake=True
    a.tgp_command32_regions=True
    a.tgp_native_command37=True
assert 0<a.frames<=100000
assert not a.tgp_native_io or a.tgp_native_jit, '--tgp-native-io requires --tgp-native-jit'
assert not a.tgp_vfp_cache or a.tgp_native_jit, '--tgp-vfp-cache requires --tgp-native-jit'
assert not(a.tgp_native_jit and (a.tgp_interpreter or a.tgp_blocks or a.profile_tgp_pcs or a.profile_tgp_commands or a.guest_counts)), 'Native TGP translation cannot be combined with interpreter/block execution or DSP guest/command tracing'
if a.resumable_tgp_rotation and not (a.native_tgp_rotation and a.resumable_matrix):
    p.error('--resumable-tgp-rotation requires --native-tgp-rotation and --resumable-matrix')
if a.native_tgp_translation and not a.native_matrix:
    p.error('--native-tgp-translation requires --native-matrix')
if a.native_tgp_rotation and not a.native_matrix:
    p.error('--native-tgp-rotation requires --native-matrix')
if a.s24_neon_decode is None:a.s24_neon_decode=bool(a.fast and a.mode!='simulation')
if a.palette_shade_cache is None:a.palette_shade_cache=bool(a.fast and a.batched and a.mode!='simulation')
if a.cache_2d_pens is None:a.cache_2d_pens=bool(a.fast and a.mode=='full' and a.canvas)
if a.coalesce_line_scroll is None:a.coalesce_line_scroll=bool(a.fast and a.mode=='full' and a.canvas)
if a.gpu_line_scroll is None:a.gpu_line_scroll=bool(a.fast and a.mode=='full' and a.canvas)
if a.gpu_line_scroll_opaque_only is None:a.gpu_line_scroll_opaque_only=a.gpu_line_scroll
if a.wide_palette_atlas is None:a.wide_palette_atlas=bool(a.fast and a.mode=='full')
if a.rotate_tile_maps is None:a.rotate_tile_maps=bool(a.fast and a.mode=='full' and a.canvas and a.packed_tile_map)
if a.rotate_3d_palette is None:a.rotate_3d_palette=bool(a.fast and a.mode=='full' and a.wide_palette_atlas)
if a.track_geometry_pages is None:a.track_geometry_pages=bool(a.fast and a.geometry_worker)
if a.profile_geometry and (a.interactive or not a.geometry_worker or a.mode=='graphics'):
    p.error('--profile-geometry requires a finite run with the geometry worker')
assert not a.geometry_worker or a.camera_packets, '--geometry-worker requires camera packets'
if (a.draw_cadence or a.paced) and a.mode!='full':
    p.error('--draw-cadence and --paced require full gameplay')
if a.interactive and (a.frame_trace or a.profile_simulation_windows or a.profile_render_windows):
    p.error('Frame trace and windowed samples require a finite run')
if a.shared_camera and (a.mode!='full' or not a.camera_packets or not a.compact_camera or a.mesh_render or a.mesh_handoff):
    p.error('--shared-camera requires compact camera packets without the mesh diagnostics')
if a.reference_camera_sort and (a.mode!='full' or not a.camera_packets or not a.direct_projection):
    p.error('--reference-camera-sort requires direct camera projection')
if a.linear_upscale and (not a.canvas or a.mode=='simulation'):
    p.error('--linear-upscale requires canvas rendering')
if a.mesh_render and (not a.fast or a.mode!='full' or not a.camera_packets or not a.compact_camera or not a.compact_packets or not a.indexed or not a.direct_pack or a.check_direct_pack):
    p.error('--mesh-render requires the indexed compact direct-pack full-render preset')
if a.mesh_handoff and (a.mode!='full' or not a.camera_packets or not a.compact_camera):
    p.error('--mesh-handoff requires compact camera packets in full render mode')
if a.atlas_layout and (a.interactive or a.mode!='full' or not a.batched):
    p.error('--atlas-layout requires a finite batched full-render run')
can_preload=bool(a.boot and a.mode=='full' and a.batched and a.gpu_queue and not a.draw_cadence
                 and not a.mesh_render and not a.mesh_handoff)
if a.atlas_state_plan and (not a.atlas_state_plan.is_file() or not can_preload or a.no_atlas_preload):
    p.error('--atlas-state-plan requires a full-rate batched cold start with the GPU queue and preload enabled')
if a.fast and can_preload and not a.no_atlas_preload and not a.atlas_state_plan:
    bundled_plan=pathlib.Path(__file__).resolve().with_name('atlas-preload.plan')
    if bundled_plan.is_file():a.atlas_state_plan=bundled_plan
assert not a.interactive or a.mode=='full'
assert not(a.boot and a.mode=='graphics')
assert a.fifo_batch==1 or a.cooperative_fifo or a.timed_cooperative_fifo, '--fifo-batch requires a cooperative FIFO path'
assert not a.guest_counts or (not a.interactive and a.frames<=1200)
def say(s):print(time.strftime('%H:%M:%S'),s,flush=True)
c=Client()
downloaded=[]

def download(remote,local):
    c.download(remote,local);downloaded.append((remote,local))

def clean_archived_downloads():
    import zlib
    freed=0
    for remote,local in downloaded:
        assert remote.startswith('/tmp/vf2-iteration-')
        data=local.read_bytes();crc=f'{zlib.crc32(data):08x}'
        if crc not in execute('/bin/crc32 '+shlex.quote(remote)).lower():raise RuntimeError('Archived download CRC mismatch: '+remote)
        execute('rm -f '+shlex.quote(remote));freed+=len(data)
    downloaded.clear()
    if freed:say(f'Freed {freed} RAM filesystem bytes after CRC-verified archival')
def execute(cmd):
    status,data=c.execute(cmd)
    if status:raise RuntimeError(f'{cmd}: {status}: {data.decode(errors="replace")}')
    return data.decode(errors='replace')
def state(pid):
    raw=execute(f'cat /proc/{pid}/stat 2>/dev/null || echo MISSING').strip()
    if raw=='MISSING':return None
    fields=raw.rsplit(') ',1)[1].split();return {'pid':pid,'state':fields[0],'parent':int(fields[1]),'start':int(fields[19])}
def upload(local,remote,executable=False,keep=False):
    data=local.read_bytes()
    import zlib
    crc=f'{zlib.crc32(data):08x}'
    if keep and crc in execute(f'if test -f {remote}; then /bin/crc32 {remote}; fi').lower():return
    size,sent_crc=c.upload(local,remote)
    if sent_crc!=zlib.crc32(data) or crc not in execute('/bin/crc32 '+remote).lower():raise RuntimeError('Upload CRC mismatch')
    if executable:c.chmod(remote)
    say(f'Uploaded {local.name}, CRC verified ({size} bytes)')
try:
    record_path=OUT/'iteration.json'
    prior_path=record_path if record_path.exists() else OUT/'fixed-interactive.json'
    if prior_path.exists():
        prior=json.loads(prior_path.read_text())
        prior_pid=int(prior['pid'])
        stats=execute('for p in /proc/[0-9]*/stat; do cat "$p" 2>/dev/null || true; done')
        for raw in stats.splitlines():
            if ') ' not in raw:continue
            fields=raw.rsplit(') ',1)[1].split();pid=int(raw.split(' ',1)[0])
            if int(fields[1])!=prior_pid or fields[0] in ('Z','X'):continue
            if '/run/vf2/vf2-acm' not in execute(f'cat /proc/{pid}/cmdline').split('\0'):continue
            current=state(pid)
            if not current or current['start']!=int(fields[19]):raise RuntimeError('Process identity changed')
            execute(f'kill -TERM {pid}');say(f'Stopping previous game PID {pid}')
            for _ in range(100):
                now=state(pid)
                if not now or now['state'] in ('Z','X'):break
                time.sleep(.2)
            else:raise RuntimeError('Previous game still running; no duplicate launched')
        prior_parent=state(prior_pid)
        prior_artifact=pathlib.Path(prior.get('artifacts','')).resolve()
        if (not prior_parent or prior_parent['state'] in ('Z','X')) and prior_artifact.parent==(OUT/'iterations').resolve():
            prior_stem='/tmp/vf2-iteration-'+prior_artifact.name
            for key,suffix,local in (('log','.log','game.log'),('exit_file','.exit','exit-status.txt')):
                remote=prior_stem+suffix
                if prior.get(key)==remote and execute(f'if test -f {shlex.quote(remote)}; then echo PRESENT; fi').strip()=='PRESENT':
                    prior_artifact.mkdir(exist_ok=True)
                    download(remote,prior_artifact/local)
            clean_archived_downloads()
    upload(a.binary,'/run/vf2/vf2-acm',True)
    upload(BASE/'tools/acm/run-on-device.sh','/run/vf2/run-game',True)
    if not a.boot:upload(a.state,'/run/vf2/fight.checkpoint',keep=True)
    if a.input_script:upload(a.input_script,'/run/vf2/input-script.txt')
    upload(BASE/'roms/vf2.zip','/run/vf2/vf2.zip')
    tag=time.strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex[:6]
    artifact=OUT/'iterations'/tag;artifact.mkdir(parents=True)
    symbols=a.binary.parent.parent/(a.binary.name+'.symbols.txt')
    if symbols.exists():(artifact/'symbols.txt').write_bytes(symbols.read_bytes())
    elf=a.binary.parent.parent/(a.binary.name+'.symbols')
    if a.profile and elf.exists():(artifact/'symbols.elf').write_bytes(elf.read_bytes())
    stem='/tmp/vf2-iteration-'+tag;log=stem+'.log';end=stem+'.exit'
    state_arg='' if a.boot else '--load-state /run/vf2/fight.checkpoint '
    report_every=600 if a.interactive and not a.profile else min(60,a.frames)
    command=f'/run/vf2/run-game --release-rom --release-state {"" if a.sound_board else "--no-sound-board "}{"--mute " if a.audio_mute else ""}{state_arg}--report-every {report_every}'
    if a.input_script:command+=' --script /run/vf2/input-script.txt'
    if a.mode!='full':command+=' --'+('simulation-only' if a.mode=='simulation' else 'graphics-only')
    if a.draw_cadence:command+=f' --draw-cadence {a.draw_cadence}'
    if not a.interactive:
        if not a.paced:command+=' --uncapped'
        command+=f' --frames {a.frames}'
        if a.mode!='simulation':command+=f' --capture {stem}.png'
    if a.mesh_render:command='VF2_MESH_RENDER=1 VF2_GLES_STATS=1 '+command
    if a.mesh_handoff:command='VF2_MESH_HANDOFF=1 '+command
    if a.atlas_layout:command=f'VF2_ATLAS_LAYOUT={stem}.atlas VF2_GLES_STATS=1 '+command
    if a.hash_palette_colours:command='VF2_PALETTE_COLOR_HASH=1 '+command
    if a.wide_palette_atlas:command='VF2_WIDE_PALETTE_ATLAS=1 '+command
    if a.s24_neon_decode:command='VF2_S24_NEON_DECODE=1 '+command
    if a.atlas_state_plan:
        upload(a.atlas_state_plan,stem+'.prewarm')
        command=f'VF2_ATLAS_STATE_PLAN={stem}.prewarm VF2_ATLAS_SNAPSHOT_DECODE=1 '+command
    if a.palette_shade_cache:command='VF2_PALETTE_SHADE_CACHE=1 '+command
    if a.profile_palette_churn:command='VF2_PALETTE_CHURN=1 '+command
    if a.fixed_atlas_luma:command='VF2_FIXED_ATLAS_LUMA=1 '+command
    if a.packed_tile_map:command='VF2_PACKED_TILE_MAP=1 '+command
    if a.packed_palette_index:command='VF2_PACKED_PALETTE_INDEX=1 '+command
    if a.frame_budget_division:command='VF2_FRAME_BUDGET_DIVISION=1 '+command
    if a.skip_inert_handoffs:command='VF2_SKIP_INERT_HANDOFFS=1 '+command
    if a.rotate_tile_maps:command='VF2_QUADRANT_TILE_TEXTURES=1 VF2_ROTATE_TILE_TEXTURES=1 '+command
    if a.rotate_3d_palette:command='VF2_ROTATE_3D_PALETTE=1 '+command
    if a.trace_upload_stalls:command='VF2_TRACE_MAP_UPLOADS=1 VF2_TRACE_SLOW_SUBMITS=1 '+command
    if a.cached_dsp_entry:command="VF2_TGP_CACHED_ENTRY=1 "+command
    if a.packed_dsp_handlers:command="VF2_TGP_PACKED_HANDLERS=1 "+command
    if a.frame_trace:command=f'VF2_FRAME_TRACE={stem}.frames '+command
    if a.profile_simulation_windows or a.profile_render_windows:command='VF2_CPU_SAMPLE_WINDOWS=1 '+command
    if a.cell_index_transforms:command='VF2_CELL_INDEX_TRANSFORMS=1 '+command
    if a.retained_fan_cell_bounds:command='VF2_RETAINED_FAN_CELL_BOUNDS=1 '+command
    if a.retained_fans:command='VF2_RETAINED_FANS=1 '+command
    if a.origin_packet_fan:command='VF2_ORIGIN_PACKET_FAN=1 '+command
    if a.limit_mip_scan:command='VF2_LIMIT_MIP_SCAN=1 '+command
    if a.keep_inactive_packet_slots:command='VF2_KEEP_INACTIVE_PACKET_SLOTS=1 '+command
    if a.fused_uv_bounds:command='VF2_FUSED_UV_BOUNDS=1 '+command
    if a.fast_viewport_bounds:command='VF2_FAST_VIEWPORT_BOUNDS=1 '+command
    if a.packet_chunk_capacity:command='VF2_PACKET_CHUNK_CAPACITY=1 '+command
    if a.shared_packet_storage:command='VF2_SHARED_PACKET_STORAGE=1 '+command
    if a.packet_trim_test:command='VF2_PACKET_TRIM_TEST=1 '+command
    if a.profile:command=f'VF2_CPU_SAMPLES={stem}.samples VF2_GLES_STATS=1 '+command
    if a.profile_copy_callers:command='VF2_CPU_COPY_CALLERS=1 '+command
    if a.profile_simulation:command='VF2_CPU_SAMPLES_THREAD=1 '+command
    if a.profile_tgp_commands:command='VF2_TGP_COMMAND_SAMPLES=1 '+command
    if a.profile_tgp_pcs:command='VF2_TGP_PC_SAMPLES=1 '+command
    if a.extended_matrix:command='VF2_TGP_EXTENDED_MATRIX=1 '+command
    if a.resumable_matrix:command='VF2_TGP_RESUMABLE_MATRIX=1 '+command
    if a.native_matrix:command='VF2_TGP_NATIVE_MATRIX=1 '+command
    if a.profile_packet_phases:command='VF2_PACKET_PHASES=1 '+command
    if a.early_tile_staging:command='VF2_EARLY_TILE_STAGING=1 '+command
    if a.hash_draw_stream:command='VF2_DRAW_STREAM_HASH=1 '+command
    if a.hash_texture_packets:command='VF2_CLIP_TRIANGLE_HASH=1 '+command
    if a.native_tgp_translation:command='VF2_TGP_NATIVE_TRANSLATION=1 '+command
    if a.native_tgp_rotation:command='VF2_TGP_NATIVE_ROTATION=1 '+command
    if a.resumable_tgp_rotation:command='VF2_TGP_RESUMABLE_ROTATION=1 '+command
    if a.prepared_draws:command='VF2_PREPARED_DRAWS=1 '+command
    if a.native_repeat_material:command='VF2_NATIVE_REPEAT_MATERIAL=1 '+command
    if a.dual_pack:command='VF2_DUAL_PACK=1 '+command
    if a.compact_packets:command='VF2_COMPACT_PACKETS=1 '+command
    if a.modular_clock:command='VF2_MODULAR_CLOCK=1 '+command
    if a.fast_sync_timers:command='VF2_SYNC_TIMER_FAST=1 '+command
    if a.scheduler_gaps:command='VF2_SCHEDULER_GAPS=1 '+command
    if a.stage_materials:command='VF2_STAGE_MATERIALS=1 '+command
    if a.scalar32:command='VF2_REC_SCALAR32=1 '+command
    if a.i960_bus_address:command='VF2_REC_BUS_ADDRESS=1 '+command
    if a.native_arithmetic:command='VF2_REC_NATIVE_ARITHMETIC=1 '+command
    if a.register_cycles:command='VF2_REC_REG_CYCLES=1 '+command
    if a.overlap_palettes:command='VF2_OVERLAP_PALETTES=1 '+command
    if a.i960_direct_branches:command='VF2_REC_DIRECT_BRANCHES=1 '+command
    if a.lazy_pc:command='VF2_REC_LAZY_PC=1 '+command
    if a.early_material:command='VF2_EARLY_MATERIAL=1 '+command
    if a.direct_geometry:command='VF2_DIRECT_GEOMETRY=1 '+command
    if a.shared_camera_colors:command='VF2_SHARED_CAMERA_COLORS=1 '+command
    if a.shared_camera:command='VF2_SHARED_CAMERA=1 '+command
    if a.compact_camera:command='VF2_COMPACT_CAMERA=1 '+command
    if a.atlas_page_cache:command='VF2_ATLAS_PAGE_CACHE=1 '+command
    if a.homogeneous_atlas_shaders:command='VF2_HOMOGENEOUS_ATLAS_SHADERS=1 '+command
    if a.textured_transparent_shader:command='VF2_TEXTURED_TRANSPARENT_SHADER=1 '+command
    if a.audio_experiment:command='VF2_AUDIO_EXPERIMENT=1 '+command
    if a.scsp_silence:command='VF2_SCSP_SILENCE=1 '+command
    if a.halt_68k:command='VF2_68K_HALT=1 '+command
    if a.force_lazy_aux:command='VF2_FORCE_LAZY_AUX=1 '+command
    if a.no_modular:command='VF2_NO_MODULAR=1 '+command
    if a.modular_sound:command='VF2_MODULAR_SOUND=1 '+command
    if a.sound_profile:command='VF2_SOUND_PROFILE=1 '+command
    if a.present_at_tick:command='VF2_PRESENT_AT_TICK=1 '+command
    if a.pace_spin_us:command=f'VF2_PACE_SPIN_US={a.pace_spin_us} '+command
    if a.flat_fragment_probe:command='VF2_FLAT_FRAGMENT_PROBE=1 '+command
    if a.compact_cell_vertices:command='VF2_COMPACT_CELL_VERTICES=1 '+command
    if a.projected_stream:command='VF2_PROJECTED_STREAM=1 '+command
    if a.atlas_tile_edges:command='VF2_ATLAS_TILE_EDGES=1 '+command
    if a.cell_vertex_stats:command='VF2_CELL_VERTEX_STATS=1 '+command
    if a.cooperative_fifo:command='VF2_COOPERATIVE_FIFO=1 '+command
    if a.timed_cooperative_fifo:command='VF2_COOPERATIVE_TIMED=1 VF2_COOPERATIVE_FIFO=1 '+command
    if a.fifo_batch>1:command=f'VF2_COOPERATIVE_BATCH={a.fifo_batch} '+command
    if a.profile_render_stages or a.serialize_gpu_passes:command='VF2_RENDER_STAGES=1 '+command
    if a.serialize_gpu_passes:command='VF2_SERIAL_GPU_PASSES=1 '+command
    if a.profile_geometry:command='VF2_CPU_SAMPLES_GEOMETRY=1 VF2_CPU_SAMPLES_THREAD=1 '+command
    if a.profile_render:command='VF2_CPU_SAMPLES_RENDER=1 VF2_CPU_SAMPLES_THREAD=1 '+command
    if a.direct_pack:command='VF2_GLES_DIRECT_PACK=1 '+command
    if a.check_direct_pack:command='VF2_CHECK_DIRECT_PACK=1 '+command
    if a.guest_counts:command=f'VF2_GUEST_COUNTS={stem}.guest '+command
    if a.sync_gpu:command='VF2_SYNC_GPU=1 '+command
    if a.batched:command='VF2_GLES_BATCHED=1 '+command
    if a.indexed:command='VF2_GLES_INDEXED=1 '+command
    if a.stack_packets:command='VF2_GLES_STACK_PACKETS=1 '+command
    if a.gpu2d:command='VF2_GPU2D=1 '+command
    if a.pipeline:command='VF2_PIPELINE=1 '+command
    if a.pipeline_render:command='VF2_PIPELINE_RENDER=1 '+command
    if a.geometry_neon:command='VF2_GEOMETRY_NEON=1 '+command
    if a.canvas:command='VF2_GPU_CANVAS=1 '+command
    if a.reference_camera_clip:command='VF2_REFERENCE_CAMERA_CLIP=1 '+command
    if a.reference_camera_sort:command='VF2_REFERENCE_CAMERA_SORT=1 '+command
    if a.direct_projection:command='VF2_DIRECT_PROJECTION=1 '+command
    if a.client_arrays:command='VF2_CLIENT_ARRAYS=1 '+command
    if a.track_geometry_pages:command='VF2_TRACK_GEOMETRY_PAGES=1 '+command
    if a.geometry_worker:command='VF2_GEOMETRY_WORKER=1 '+command
    if a.camera_packets:command='VF2_CAMERA_PACKETS=1 '+command
    if a.i960_timer_reciprocal:command='VF2_I960_TIMER_RECIPROCAL=1 '+command
    if a.reference_division:command='VF2_REFERENCE_DIVISION=1 '+command
    if a.reference_duration:command='VF2_REFERENCE_DURATION=1 '+command
    if a.reference_timers:command='VF2_REFERENCE_TIMERS=1 '+command
    if a.lazy_aux_clocks:command='VF2_LAZY_AUX_CLOCKS=1 '+command
    if a.cache_2d_pens:command='VF2_CACHE_2D_PENS=1 '+command
    if a.hash_2d_pens:command='VF2_HASH_2D_PENS=1 '+command
    if a.coalesce_line_scroll:command='VF2_COALESCE_LINE_SCROLL=1 '+command
    if a.gpu_line_scroll and a.gpu_line_scroll_opaque_only:command='VF2_GPU_LINE_SCROLL_OPAQUE_ONLY=1 '+command
    if a.gpu_line_scroll:command='VF2_GPU_LINE_SCROLL=1 '+command
    if a.clip_empty_tiles:command='VF2_CLIP_EMPTY_TILES=1 '+command
    if a.linear_upscale:command='VF2_LINEAR_UPSCALE=1 '+command
    if a.swap_interval is not None:command=f'VF2_SWAP_INTERVAL={a.swap_interval} '+command
    if a.reference_packet_math:command='VF2_REFERENCE_PACKET_MATH=1 '+command
    if a.reference_geometry_push:command='VF2_REFERENCE_GEOMETRY_PUSH=1 '+command
    if a.single_tgp:command='VF2_TGP_SINGLE=1 '+command
    if a.reference_tgp_counter:command='VF2_TGP_COUNTER_REFERENCE=1 '+command
    if a.tgp_native_io:command='VF2_TGP_NATIVE_IO=1 '+command
    if a.tgp_vfp_cache:command='VF2_TGP_VFP_CACHE=1 '+command
    if a.tgp_native_jit:command='VF2_TGP_NATIVE_JIT=1 '+command
    if a.tgp_4a_resume:command='VF2_TGP_4A_RESUME=1 '+command
    if a.tgp_native_command37:command='VF2_TGP_NATIVE_COMMAND37=1 '+command
    if not a.tgp_private_regions:command='VF2_NO_TGP_PRIVATE_REGIONS=1 '+command
    if a.i960_direct_register_frame:command='VF2_I960_DIRECT_REGISTER_FRAME=1 '+command
    if a.i960_port_stores:command='VF2_REC_PORT_STORES=1 '+command
    if a.i960_predecoded_bursts:command="VF2_REC_PREDECODED_BURSTS=1 "+command
    if a.i960_fixed_fifo_load:command='VF2_I960_FIXED_FIFO_LOAD=1 '+command
    if a.i960_fixed_fifo_store:command='VF2_I960_FIXED_FIFO_STORE=1 '+command
    if a.fuse_fifo_halt:command='VF2_FUSE_FIFO_HALT=1 '+command
    if a.fast_halt_delivery:command='VF2_FAST_HALT_DELIVERY=1 '+command
    if a.tgp_native_dispatch:command='VF2_TGP_NATIVE_DISPATCH=1 '+command
    if a.tgp_fused_command_intake:command='VF2_TGP_FUSED_COMMAND_INTAKE=1 '+command
    if a.tgp_command32_regions:command='VF2_TGP_COMMAND32_REGIONS=1 '+command
    if a.tgp_resume_dispatch:command='VF2_TGP_RESUME_DISPATCH=1 '+command
    if a.tgp_read_window:command='VF2_TGP_READ_WINDOW=1 '+command
    if a.tgp_stable_program:command='VF2_TGP_STABLE_PROGRAM=1 '+command
    if a.tgp_blocks:command='VF2_TGP_BLOCKS=1 '+command
    if a.guarded_tgp:command='VF2_TGP_FETCH_GUARDS=1 '+command
    if a.gpu_queue:command='VF2_GPU_QUEUE=1 '+command
    if a.atomic_row_claims:command='VF2_ATOMIC_ROW_CLAIMS=1 '+command
    if a.direct_task_ranges:command='VF2_DIRECT_TASK_RANGES=1 '+command
    if a.render_threads:command=f'VF2_THREADS={a.render_threads} '+command
    if a.sim_cpu is not None:command=f'VF2_SIM_CPU={a.sim_cpu} '+command
    if a.full_texture_snapshot:command='VF2_FULL_TEXTURE_SNAPSHOT=1 '+command
    if a.slow_tgp_ram:command='VF2_TGP_SLOW_RAM=1 '+command
    if a.tgp_interpreter:command='VF2_TGP_INTERP=1 '+command
    pid=int(execute(f'( {command} > {log} 2>&1; echo $? > {end} ) < /dev/null > /dev/null 2>&1 & echo $!').strip())
    record={'pid':pid,'log':log,'exit_file':end,'command':command,'mode':a.mode,'boot':a.boot,'artifacts':str(artifact),'binary':str(a.binary),'binary_sha256':hashlib.sha256(a.binary.read_bytes()).hexdigest(),'state_sha256':None if a.boot else hashlib.sha256(a.state.read_bytes()).hexdigest(),'script_sha256':hashlib.sha256(a.input_script.read_bytes()).hexdigest() if a.input_script else None}
    record_path.write_text(json.dumps(record,indent=2));(artifact/'job.json').write_text(json.dumps(record,indent=2));say(f'Started {"cold-start" if a.boot else "checkpoint"} iteration, PID {pid}')
    deadline=time.monotonic()+max(180,a.frames*2);last='';gpu_runtime=[]
    if a.quiet_monitor:
        # Startup, the final capture and ordinary stalls fit inside the extra wait.
        time.sleep(a.frames*.017384+20)
    first_poll=time.monotonic()
    record['quiet_monitor']=a.quiet_monitor
    record['first_poll_after_launch_seconds']=first_poll-(deadline-max(180,a.frames*2))
    while True:
        if a.profile:
            diagnostics=execute('for p in /sys/kernel/debug/mali/utilization_gp_pp /sys/kernel/debug/mali/utilization_gp /sys/kernel/debug/mali/utilization_pp /sys/kernel/debug/clk/hosc/pll_gpu/gpu/clk_rate /sys/class/devfreq/sunxi-ddrfreq/cur_freq /proc/meminfo; do if test -f $p; then echo $p; cat $p; fi; done')
            gpu_runtime.append({'time':time.time(),'values':diagnostics})
            (artifact/'gpu-runtime.json').write_text(json.dumps(gpu_runtime,indent=2))
        text=execute('cat '+log)
        if text!=last:print(text[len(last):],end='',flush=True);last=text;(artifact/'game.log').write_text(text)
        finished=execute(f'if test -f {end}; then cat {end}; else echo RUNNING; fi').strip()
        parent=state(pid)
        if finished!='RUNNING' and (not parent or parent['state'] in ('Z','X')):
            record['exit_status']=int(finished);record_path.write_text(json.dumps(record,indent=2));(artifact/'job.json').write_text(json.dumps(record,indent=2))
            download(log,artifact/'game.log')
            download(end,artifact/'exit-status.txt')
            if a.atlas_state_plan:execute('rm -f '+shlex.quote(stem+'.prewarm'))
            if finished!='0':
                clean_archived_downloads()
                raise RuntimeError('Iteration failed: '+finished)
            if a.mode!='simulation':download(stem+'.png',artifact/'screen.png')
            if a.frame_trace:download(stem+'.frames',artifact/'frames.csv')
            if a.profile_simulation_windows or a.profile_render_windows:download(stem+'.samples.windows',artifact/'cpu.samples.windows')
            if a.atlas_layout:download(stem+'.atlas',artifact/'atlas-layout.txt')
            if a.profile:
                download(stem+'.samples',artifact/'cpu.samples');download(stem+'.samples.maps',artifact/'cpu.samples.maps')
                download(stem+'.samples.jit',artifact/'cpu.samples.jit')
                if a.profile_copy_callers:download(stem+'.samples.copy-callers',artifact/'cpu.samples.copy-callers')
                if a.profile_simulation or a.profile_render or a.profile_geometry:download(stem+'.samples.thread',artifact/'cpu.samples.thread')
                if a.profile_tgp_pcs:
                    download(stem+'.samples.tgp',artifact/'cpu.samples.tgp')
                if a.profile_tgp_pcs or a.profile_tgp_commands:
                    download(stem+'.samples.tgp-commands',artifact/'cpu.samples.tgp-commands')
            if a.guest_counts:
                for suffix in ('i960','tgp','commands'):download(stem+'.guest.'+suffix,artifact/('guest.'+suffix))
            clean_archived_downloads()
            say('Saved iteration results to '+str(artifact));break
        if (a.interactive and ('Resumed checkpoint' in text or (a.boot and 'Input /dev/input/' in text))
                and (not a.atlas_state_plan or 'Atlas preload plan ready:' in text)):
            if a.atlas_state_plan:execute('rm -f '+shlex.quote(stem+'.prewarm'))
            say('Interactive game running, audio deferred');break
        if time.monotonic()>deadline:raise RuntimeError(f'Observation deadline; PID {pid} was not restarted')
        time.sleep(2)
finally:c.close()
