-- Movie check, on the disc tests/boot/mkdisc.py makes: boot.sh passes when "BOOT OK: movies" is printed.
-- CLIP plays full screen through Movie.Play, then TV on this crate through Movie.PlayOn. CI also greps
-- the engine's own count for each, so a player that runs but shows nothing still fails.
local frames, played, onCrate, done = 0, false, false, false

function onUpdate(self, dt)
    frames = frames + 1
    if frames == 5 then
        if not Movie.Play("CLIP", { onComplete = function() played = true end }) then
            Debug.Log("BOOT FAIL: Movie.Play did not start")
        end
    end
    if played and not onCrate then
        onCrate = true
        if not Movie.PlayOn(Entity.Find("Crate"), "TV", { fps = 15, loop = false, onComplete = function() done = true end }) then
            Debug.Log("BOOT FAIL: Movie.PlayOn did not start")
        end
    end
    if done then
        done = false
        Debug.Log("BOOT OK: movies")
    end
    if frames == 1200 then
        Debug.Log("BOOT FAIL: movies, played=" .. tostring(played) .. " on the crate=" .. tostring(onCrate))
    end
end
