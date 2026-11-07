-- On the crate of particles.scene: at update 240 Fountain has filled its pool
-- (24 a second for 1.5 s: 36 alive); Lua stops it and plays Sparks. 120
-- updates later every Fountain particle has lived out its 1.5 s: none left
-- and not playing prints BOOT OK: particles.
local n = 0
function onUpdate(self, dt)
    n = n + 1
    if n == 240 then
        local live = Particles.Count("Fountain")
        Debug.Log("PARTICLES STOP at update " .. n .. " live " .. live)
        if live < 30 then Debug.Log("BOOT FAIL particles: only " .. live .. " alive at the stop") end
        Particles.Stop("Fountain")
        Particles.Play("Sparks")
    elseif n == 360 then
        local live, playing = Particles.Count("Fountain"), Particles.IsPlaying("Fountain")
        if live == 0 and not playing then
            Debug.Log("BOOT OK: particles")
        else
            Debug.Log("BOOT FAIL particles: " .. live .. " alive after the stop, playing " .. tostring(playing))
        end
    end
end
