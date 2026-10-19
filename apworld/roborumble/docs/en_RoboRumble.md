# RoboRumble

## What does randomization do to this game?

The weapons (robot heads), legs and alien boosters you can build robots from are no longer handed out per mission;
they are items. Missions are unlocked by Mission Access items and can be played in any order, like a mission map: talk to the robot
Zuom IV (rightmost portrait in the briefing) and pick "Choose a mission". The final mission (16) has no access item;
it opens after a number of won missions of the same side (option `final_mission_wins`).

## What is the goal?

Win the final mission against the Aliens (mission 16) - of both campaigns with `side: both`.

## Which locations are checks?

- winning each of the 16 missions of your side
- each robot built with one of your side's weapons or legs for the first time (when it leaves the factory)
- each booster used on a built robot for the first time (option `booster_builds`)
- every weapon + legs combination (option `combo_builds`)
- every purchase from the engineer: boosters, base enhancements, secret inventions (option `shop_checks`)
- the spy's report before each mission (option `spy_checks`, optionally also a hint)

## What does another world's item look like in RoboRumble?

A message in the game; the parts appear in the robot builder as soon as they arrive.
