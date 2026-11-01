# Starfield Advanced DualSense (SAD)

A fork of [Starfield Advanced DualSense](https://github.com/VexenDeimos/Starfield-Advanced-DualSense) that adds fixes on top of each upstream release.
For features, installation, and settings, see the [original README](https://github.com/VexenDeimos/Starfield-Advanced-DualSense/blob/v0.8.0/README.md).

## Changes from upstream

- Avoids possible memory corruption when a controller disconnects or a speaker write times out.
- Voice lines on the controller speaker use loose-file replacements, and ship-radio lines are decoded in the background instead of delaying controller updates.
- Stops reading player data while a save loads or the game returns to the main menu, when that data may be invalid.
- Gameplay vibration and trigger effects follow the same menu rules on foot, in ships, and in vehicles, so they stop in the star map and stay stopped while the game is paused.
