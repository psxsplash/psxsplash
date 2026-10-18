-- Cutscene check: boot.sh passes when "BOOT OK: cutscenes" is printed.
-- The loop below makes every frame slower than four frame times, so dt is
-- capped at four frames and a cutscene steps 0, 4, ..., 60, 64. Both
-- cutscenes are 62 frames long with a key on frame 62, which no tick lands on.
-- "hide" chains "show" from its onComplete.
local frames, crate = 0, nil
local hidden, shown, showDoneAt = nil, nil, nil

function onUpdate(self, dt)
    frames = frames + 1
    local n = 0
    for i = 1, 300000 do n = n + 1 end
    if frames <= 3 then Debug.Log("CUTSCENE dt=" .. dt) end
    if frames == 5 then
        crate = Entity.Find("Crate (2)")
        Cutscene.Play("hide", { onComplete = function()
            hidden = not Entity.IsActive(crate)
            Debug.Log("CUTSCENE hide done, crate hidden=" .. tostring(hidden))
            Cutscene.Play("show", { onComplete = function()
                shown = Entity.IsActive(crate)
                showDoneAt = frames
                Debug.Log("CUTSCENE show done, crate shown=" .. tostring(shown))
            end })
        end })
    end
    if frames == 60 then
        if hidden == true and shown == true then
            Debug.Log("BOOT OK: cutscenes")
        else
            Debug.Log("BOOT FAIL: cutscenes hidden=" .. tostring(hidden) .. " shown=" .. tostring(shown) ..
                " playing=" .. tostring(Cutscene.IsPlaying()))
        end
    end
end
