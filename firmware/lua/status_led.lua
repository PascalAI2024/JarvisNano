--[[
  JarvisNano — Status LED (Phase 1)

  Drives the on-board user LED of the Seeed XIAO ESP32-S3 Sense
  (GPIO21, active-LOW) with binary on/off patterns. We use direct gpio
  control with short duty-cycle frames, so the single LED gets a
  soft "alive" pulse without requiring PWM hardware.

  State patterns:
    BOOT       quick wake flash + soft settle pulse
    IDLE       soft double heartbeat every ~2 s ("I'm alive")
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

local BOOT_SETTLE_PULSE = {2, 4, 7, 11, 15, 18, 15, 11, 7, 4, 2}
local IDLE_MAIN_PULSE   = {2, 4, 7, 11, 15, 18, 15, 11, 7, 4, 2}
local IDLE_ECHO_PULSE   = {2, 5, 9, 12, 9, 5, 2}

local function led(level) gpio.set_level(PIN, level) end
local function on()       led(LED_ON)  end
local function off()      led(LED_OFF) end

local function blink(times, on_ms, off_ms)
    for _ = 1, times do
        on();  delay.delay_ms(on_ms)
        off(); delay.delay_ms(off_ms)
    end
end

local function glow_frame(on_ms, frame_ms)
    if on_ms > 0 then
        on()
        delay.delay_ms(on_ms)
    end
    if frame_ms > on_ms then
        off()
        delay.delay_ms(frame_ms - on_ms)
    end
end

local function soft_pulse(levels, frame_ms)
    for i = 1, #levels do
        glow_frame(levels[i], frame_ms)
    end
    off()
end

-- ---------------------------------------------------------------
-- State runners (each runs one full cycle; main loop repeats)
-- ---------------------------------------------------------------

-- Wake flash, then a soft settle pulse so power-on is visible at a glance.
local function run_boot()
    blink(2, 70, 80)
    soft_pulse(BOOT_SETTLE_PULSE, 18)
    delay.delay_ms(120)
    blink(1, 220, 180)
    STATE = "IDLE"
end

-- Soft double heartbeat. On a single active-low LED this reads as
-- "powered, booted, and waiting" without looking like an error blink.
local function run_idle()
    soft_pulse(IDLE_MAIN_PULSE, 18)
    delay.delay_ms(120)
    soft_pulse(IDLE_ECHO_PULSE, 16)
    delay.delay_ms(1500)
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
