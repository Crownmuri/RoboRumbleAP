# RoboRumble

## What does randomization do to this game?

The weapons (robot heads), legs and alien boosters you can build robots from are no longer handed out per mission;
they are items. Missions are unlocked by Mission Access items and can be played in any order, like a mission map: talk to the robot
Zuom IV (rightmost portrait in the briefing) and pick "Choose a mission". The final mission (16) has no access item;
it opens after a number of won missions of the same side (option `final_mission_wins`).

Other items:
- **Progressive Starting Robo Funds**: +400 Robo Funds (the money for building robots during a mission) at the start
  of every mission, per copy (option `starting_robo_funds`)
- **Money** ("$250 000 Money" up to "$2 000 000 Money", the name says how much): to spend in the engineer's shop between
  missions
- base enhancements and secret inventions from the shop, Alien Artifacts (artifact goals), traps (option
  `trap_percentage`)

With `unholy_alliance` the two sides' weapons and legs are mixed: Red Star can build a Laser on Spider Legs and
Terraflux a Chain Gun on 4-Tires. `switch` swaps some builder slots with the same slot of the other side,
`full_shuffle` splits all 16 weapons and 16 legs between the sides at random. Enemies keep their original parts.

## What is the goal?

Win the final mission against the Aliens (mission 16) - of both campaigns with `side: both`. With both sides, one
campaign's mission 1 is open from the start (`starting_side`) and the other needs its Mission 1 Access item; Zuom IV's
menu switches between the campaigns within one save.

## Which locations are checks?

- winning each of the 16 missions of your side
- each robot built with one of your side's weapons or legs for the first time (when it leaves the factory):
  "Build Laser"
- each booster used on a built robot for the first time (option `booster_builds`)
- every weapon + legs combination (option `combo_builds`)
- every purchase from the engineer: boosters, base enhancements, secret inventions (option `shop_checks`)
- the spy's report before each mission (option `spy_checks`, optionally also a hint)

## What does another world's item look like in RoboRumble?

A message in the game; the parts appear in the robot builder as soon as they arrive.

## Dev commands

With `dev_mode: true` under `roborumble_options` in `host.yaml`, the client's `/dev` commands help testing:
traps, winning or losing the mission, Money, giving yourself items in the game only, god mode (`/dev god`), no lost
missions (`/dev nolose`), endless Robo Funds (`/dev funds`) and Money (`/dev infmoney`), and alien boosters
(`/dev alien speed`). `/dev help` lists them.
