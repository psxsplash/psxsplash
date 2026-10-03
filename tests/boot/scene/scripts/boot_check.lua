-- Boot check: tests/boot/boot.sh passes when this line is printed.
local frames = 0

function onUpdate(self, dt)
    frames = frames + 1
    if frames == 60 then
        Debug.Log("BOOT OK: 60 frames")
    end
end
