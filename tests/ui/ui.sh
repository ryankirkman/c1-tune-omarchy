# Helpers for driving the real panel and bar button with real pointer and keyboard input,
# on Hyprland. Source this after `make build/ptr`; needs grim, wtype and a running session.
#   open_panel; click 294 54; drag 125 432 251; get saturation; shot name
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
P=$ROOT/build/ptr
screen() { hyprctl monitors -j | python3 -c "import json,sys; m=json.load(sys.stdin)[0]; print(int(m['width']/m['scale']), int(m['height']/m['scale']))"; }
read SW SH <<< "$(screen)"
# The panel's x y width height. It is a layer surface (a popover), or an ordinary window
# when run with C1TUNE_PANEL_WINDOW=1.
win() { python3 - <<'PY'
import json, subprocess
def run(*a): return json.loads(subprocess.run(a, capture_output=True, text=True).stdout or 'null')
for monitor in (run('hyprctl', 'layers', '-j') or {}).values():
    for level in monitor['levels'].values():
        for layer in level:
            if layer['namespace'] == 'c1-tune':
                print(layer['x'], layer['y'], layer['w'], layer['h']); raise SystemExit
for c in run('hyprctl', 'clients', '-j') or []:
    if c['class'] == 'c1-tune':
        print(c['at'][0], c['at'][1], c['size'][0], c['size'][1]); break
PY
}
open_panel() { [ -z "$(win)" ] && { (c1-tune --panel >/dev/null 2>&1 &); for i in $(seq 1 50); do [ -n "$(win)" ] && break; sleep 0.1; done; sleep 0.6; }; }
# Rows sit lower when the preview or an error line is showing: measure, don't assume.
at() { read wx wy ww wh <<< "$(win)"; y=$2; [ $y -gt 40 ] && y=$((y + wh - 786)); echo $((wx+$1)) $((wy+y)); }
click() { [ -z "$(win)" ] && { echo "  (no panel window: click skipped)"; return; }; read x y <<< "$(at $1 $2)"; $P $SW $SH click $x $y; sleep ${3:-0.35}; }
drag() { [ -z "$(win)" ] && { echo "  (no panel window: drag skipped)"; return; }; read x1 y1 <<< "$(at $1 $2)"; read x2 y2 <<< "$(at $3 $2)"; $P $SW $SH drag $x1 $y1 $x2 $y1; sleep ${4:-0.4}; }
shot() { read wx wy ww wh <<< "$(win)"; mkdir -p "$ROOT/build/t"; grim -g "$wx,$wy ${ww}x${wh}" "$ROOT/build/t/$1.png"; }   # a picture of the panel
park() { $P $SW $SH move $((SW/2)) $((SH/2)); }   # get the pointer off whatever it last touched
get() { c1-tune --status | python3 -c "
import json,sys
d=json.load(sys.stdin); s=d['settings']
print(' '.join('%s=%s'%(k,s[k] if k in s else d.get(k)) for k in sys.argv[1:]))" "$@"; }
ok() { if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got '$2' expected '$3'"; fi; }
# Layout in logical pixels inside the 340x786 panel (preview off). Slider tracks run x=125..251.
SX0=125; SX1=251; X_SWITCH=294
Y_EXP=54; Y_SHUTTER=82; Y_ISO=111; Y_BIAS=140; Y_FOCUS=176; Y_DIST=204; Y_WB=240; Y_TEMP=268
Y_ZOOM=321; Y_FX=350; Y_FY=379; Y_BRI=432; Y_CON=461; Y_SAT=490; Y_SHA=519; Y_DEN=548; Y_CDEN=577
Y_FLICK=617; Y_CHK1=660; Y_CHK2=711; Y_BTN=757
