-- Lua breakpoint check (tests/boot/luadebug.sh): luabp.txt sets a breakpoint
-- on the line marked BREAK; the run stops there with `answer` among the
-- locals, steps to the next line (where `marker` is a global), continues,
-- and this prints the pass line.
local frames = 0

function onUpdate(self, dt)
    frames = frames + 1
    if frames == 30 then
        local answer = 42
        marker = answer + 1 -- BREAK
        Debug.Log("BOOT OK: luadebug " .. marker)
    end
end
