--[[
  JarvisNano — Status LED (Phase 1)

  Drives the on-board user LED of the Seeed XIAO ESP32-S3 Sense
  (GPIO21, active-LOW) with binary on/off patterns. We use direct gpio
  control instead of PWM so the LED blink patterns are crisp and
  reliable.

  State patterns:
    BOOT       three quick bright flashes
    IDLE       slow heartbeat — short wink every 2 s ("I'm alive")
    LISTENING  medium blink, ~3 Hz
    THINKING   fast strobe, ~6 Hz
    SPEAKING   double-tap heartbeat
    ERROR      SOS (... --- ...)

  Phase 1 forces STATE = IDLE forever. Phase 1.5 will set STATE from
  router rules that watch claw_core agent_stage events.

  Args (all optional):
    pin        GPIO of the LED, default 21 (XIAO Sense on-board)
    active_low 1 if the LED turns on when pin is LOW (XIAO default), default 1

  Run from REPL:
    lua --run-async --path builtin/status_led.lua
]]

local gpio  = require("gpio")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local function int_arg(k, default)
    local v = a[k]
    if type(v) == "number" then return math.floor(v) end
    return default
end

local PIN        = int_arg("pin", 21)
local ACTIVE_LOW = (int_arg("active_low", 1) ~= 0)

local LED_ON  = ACTIVE_LOW and 0 or 1
local LED_OFF = ACTIVE_LOW and 1 or 0

local STATE = "BOOT"

local function led(level) gpio.set_level(PIN, level) end
local function on()       led(LED_ON)  end
local function off()      led(LED_OFF) end

local function blink(times, on_ms, off_ms)
    for _ = 1, times do
        on();  delay.delay_ms(on_ms)
        off(); delay.delay_ms(off_ms)
    end
end

-- ---------------------------------------------------------------
-- State runners (each runs one full cycle; main loop repeats)
-- ---------------------------------------------------------------

-- 3 quick bright flashes, then darkness.
local function run_boot()
    blink(3, 100, 130)
    delay.delay_ms(200)
    STATE = "IDLE"
end

-- Heartbeat: short wink, long pause. 1 cycle ~ 2 s. "Alive, awaiting input."
local function run_idle()
    on();  delay.delay_ms(120)
    off(); delay.delay_ms(1880)
end

-- Medium blink ~3 Hz. Mic is capturing.
local function run_listening()
    blink(2, 150, 150)
    delay.delay_ms(0)
end

-- Fast strobe ~6 Hz. LLM in flight.
local function run_thinking()
    blink(4, 80, 80)
end

-- Double-tap heartbeat. TTS playing.
local function run_speaking()
    on();  delay.delay_ms(60)
    off(); delay.delay_ms(120)
    on();  delay.delay_ms(60)
    off(); delay.delay_ms(900)
end

-- SOS Morse. Uncaught error or LLM failure.
local function run_error()
    blink(3, 120, 120)   -- S
    delay.delay_ms(180)
    blink(3, 360, 120)   -- O
    delay.delay_ms(180)
    blink(3, 120, 120)   -- S
    delay.delay_ms(900)
    STATE = "IDLE"
end

-- ---------------------------------------------------------------
-- Main loop
-- ---------------------------------------------------------------

local function tick()
    if     STATE == "BOOT"      then run_boot()
    elseif STATE == "IDLE"      then run_idle()
    elseif STATE == "LISTENING" then run_listening()
    elseif STATE == "THINKING"  then run_thinking()
    elseif STATE == "SPEAKING"  then run_speaking()
    elseif STATE == "ERROR"     then run_error()
    else   STATE = "IDLE"
    end
end

local function run()
    print("[status_led] on-board LED on gpio " .. tostring(PIN) .. " active_low=" .. tostring(ACTIVE_LOW))
    gpio.set_direction(PIN, "output")
    off()
    while true do tick() end
end

local ok, err = xpcall(run, debug.traceback)
pcall(off)
if not ok then
    print("[status_led] error: " .. tostring(err))
    error(err)
end
