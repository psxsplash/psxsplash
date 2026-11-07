-- Particle emitters boot check (scene script of particles.scene): both
-- emitters resolve by their objects' names (Fountain 0, Sparks 1, an unknown
-- name nil), Fountain plays from the start (autoplay) and Sparks does not, and
-- a Burst of 5 gives Sparks 5 live particles. particles_crate.lua finishes it.
function onSceneCreationEnd()
    local f, s, none = Particles.Find("Fountain"), Particles.Find("Sparks"), Particles.Find("nope")
    local fp, sp = Particles.IsPlaying("Fountain"), Particles.IsPlaying(s)
    Particles.Burst("Sparks", 5)
    local n = Particles.Count("Sparks")
    local got = "find " .. tostring(f) .. "," .. tostring(s) .. "," .. tostring(none) .. " playing " .. tostring(fp) ..
                "," .. tostring(sp) .. " burst " .. tostring(n)
    if f == 0 and s == 1 and none == nil and fp == true and sp == false and n == 5 then
        Debug.Log("PARTICLES OK " .. got)
    else
        Debug.Log("BOOT FAIL particles: " .. got)
    end
end
